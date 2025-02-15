/*
 * Copyright (C) 2016 Jared Boone, ShareBrained Technology, Inc.
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#include "ui_record_view.hpp"

#include "portapack.hpp"
using namespace portapack;

#include "io_file.hpp"
#include "io_wave.hpp"
#include "io_convert.hpp"

#include "baseband_api.hpp"
#include "metadata_file.hpp"
#include "oversample.hpp"
#include "rtc_time.hpp"
#include "string_format.hpp"
#include "utility.hpp"

#include <cstdint>

namespace ui {

/*void RecordView::toggle_pitch_rssi() {
        pitch_rssi_enabled = !pitch_rssi_enabled;

        // Send to RSSI widget
        const PitchRSSIConfigureMessage message {
                pitch_rssi_enabled,
                0
        };
        shared_memory.application_queue.push(message);

        if( !pitch_rssi_enabled ) {
                button_pitch_rssi.set_foreground(Color::orange());
        } else {
                button_pitch_rssi.set_foreground(Color::green());
        }
}*/

RecordView::RecordView(
    const Rect parent_rect,
    const std::filesystem::path& filename_stem_pattern,
    const std::filesystem::path& folder,
    const FileType file_type,
    const size_t write_size,
    const size_t buffer_count)
    : View{parent_rect},
      filename_stem_pattern{filename_stem_pattern},
      folder{folder},
      file_type{file_type},
      write_size{write_size},
      buffer_count{buffer_count} {
    ensure_directory(folder);
    add_children({
        &rect_background,
        //&button_pitch_rssi,
        &button_record,
        &text_record_filename,
        &text_record_dropped,
        &text_time_available,
    });

    rect_background.set_parent_rect({{0, 0}, size()});

    /*button_pitch_rssi.on_select = [this](ImageButton&) {
                this->toggle_pitch_rssi();
        };*/

    button_record.on_select = [this](ImageButton&) {
        this->toggle();
    };

    signal_token_tick_second = rtc_time::signal_tick_second += [this]() {
        this->on_tick_second();
    };
}

RecordView::~RecordView() {
    rtc_time::signal_tick_second -= signal_token_tick_second;
}

void RecordView::focus() {
    button_record.focus();
}

uint32_t RecordView::set_sampling_rate(uint32_t new_sampling_rate) {
    // Determine the oversampling needed (if any) and the actual sampling rate.
    auto oversample_rate = get_oversample_rate(new_sampling_rate);
    auto actual_sampling_rate = new_sampling_rate * toUType(oversample_rate);

    /* We are changing "REC" icon background to yellow in BW rec Options >1Mhz
     * > 1Mhz BW options , we are NOT recording full IQ .C16 files (those files has some periodical  missing dropped samples).
     * Those recorded files, has not the full IQ samples information, looks like decimated in file size.
     * They are ok as recorded spectrum indication, but they should not be used by Replay app. (the voice speed will be accelerated)

     * We keep original black background in all the correct IQ .C16 files BW's Options. */
    if (actual_sampling_rate > 8'000'000) {  // yellow REC button means not ok for REC, BW >1Mhz  (BW from 12k5 till 1Mhz OK for REC and Replay)
        button_record.set_background(ui::Color::yellow());
    } else {
        button_record.set_background(ui::Color::black());
    }

    if (sampling_rate != new_sampling_rate) {
        stop();

        sampling_rate = new_sampling_rate;
        baseband::set_sample_rate(sampling_rate, oversample_rate);

        button_record.hidden(sampling_rate == 0);
        text_record_filename.hidden(sampling_rate == 0);
        text_record_dropped.hidden(sampling_rate == 0);
        text_time_available.hidden(sampling_rate == 0);
        rect_background.hidden(sampling_rate != 0);

        update_status_display();
    }

    return actual_sampling_rate;
}

OversampleRate RecordView::get_oversample_rate(uint32_t sample_rate) {
    // No oversampling necessary for baseband audio processors.
    if (file_type == FileType::WAV)
        return OversampleRate::None;

    auto rate = ::get_oversample_rate(sample_rate);

    // Currently proc_capture only supports /8, /16, /32 for decimation.
    if (rate < OversampleRate::x8)  // clipping while /4 is not implemented yet  (it will be used >1Mhz onwards when available)
        rate = OversampleRate::x8;
    else if (rate > OversampleRate::x64)  // clipping while /128 is not implemented yet , (but it is not necessary for 12k5)
        rate = OversampleRate::x64;

    return rate;
}

// Setter for datetime and frequency filename
void RecordView::set_filename_date_frequency(bool set) {
    filename_date_frequency = set;
}

bool RecordView::is_active() const {
    return (bool)capture_thread;
}

void RecordView::toggle() {
    if (is_active()) {
        stop();
    } else {
        start();
    }
}

void RecordView::start() {
    stop();

    text_record_filename.set("");
    text_record_dropped.set("");

    if (sampling_rate == 0) {
        return;
    }

    std::filesystem::path base_path;
    if (filename_date_frequency) {
        rtcGetTime(&RTCD1, &datetime);

        // ISO 8601
        std::string date_time =
            to_string_dec_uint(datetime.year(), 4, '0') +
            to_string_dec_uint(datetime.month(), 2, '0') +
            to_string_dec_uint(datetime.day(), 2, '0') + "T" +
            to_string_dec_uint(datetime.hour()) +
            to_string_dec_uint(datetime.minute()) +
            to_string_dec_uint(datetime.second());

        base_path = filename_stem_pattern.string() + "_" + date_time + "_" +
                    trim(to_string_freq(receiver_model.target_frequency())) + "Hz";
        base_path = folder / base_path;
    } else {
        base_path = next_filename_matching_pattern(folder / filename_stem_pattern);
    }

    if (base_path.empty()) {
        return;
    }

    std::unique_ptr<stream::Writer> writer;
    switch (file_type) {
        case FileType::WAV: {
            auto p = std::make_unique<WAVFileWriter>();
            auto create_error = p->create(
                base_path.replace_extension(u".WAV"),
                sampling_rate,
                to_string_dec_uint(receiver_model.target_frequency()) + "Hz");
            if (create_error.is_valid()) {
                handle_error(create_error.value());
            } else {
                writer = std::move(p);
            }
        } break;

        case FileType::RawS8:
        case FileType::RawS16: {
            const auto metadata_file_error = write_metadata_file(
                get_metadata_path(base_path), {receiver_model.target_frequency(), sampling_rate});
            if (metadata_file_error.is_valid()) {
                handle_error(metadata_file_error.value());
                return;
            }

            auto p = std::make_unique<FileConvertWriter>();
            auto create_error = p->create(base_path.replace_extension((file_type == FileType::RawS8) ? u".C8" : u".C16"));
            if (create_error.is_valid()) {
                handle_error(create_error.value());
            } else {
                writer = std::move(p);
            }
        } break;

        default:
            break;
    };

    if (writer) {
        text_record_filename.set(truncate(base_path.filename().string(), 8));
        button_record.set_bitmap(&bitmap_stop);
        capture_thread = std::make_unique<CaptureThread>(
            std::move(writer),
            write_size, buffer_count,
            []() {
                CaptureThreadDoneMessage message{};
                EventDispatcher::send_message(message);
            },
            [](File::Error error) {
                CaptureThreadDoneMessage message{error.code()};
                EventDispatcher::send_message(message);
            });
    }

    update_status_display();
}

void RecordView::on_hide() {
    stop();  // Stop current recording
    View::on_hide();
}

void RecordView::stop() {
    if (is_active()) {
        capture_thread.reset();
        button_record.set_bitmap(&bitmap_record);
    }

    update_status_display();
}

void RecordView::on_tick_second() {
    update_status_display();
}

void RecordView::update_status_display() {
    if (is_active()) {
        const auto dropped_percent = std::min(99U, capture_thread->state().dropped_percent());
        const auto s = to_string_dec_uint(dropped_percent, 2, ' ') + "%";
        text_record_dropped.set(s);
    }

    /*
    if (pitch_rssi_enabled) {
        button_pitch_rssi.invert_colors();
    }
    */

    if (sampling_rate > 0) {
        const auto space_info = std::filesystem::space(u"");
        // - Audio is 1 int16_t per sample or '2' bytes per sample.
        // - C8 captures 2 (I,Q) int8_t per sample or '2' bytes per sample.
        // - C16 captures 2 (I,Q) int16_t per sample or '4' bytes per sample.
        const auto bytes_per_sample = file_type == FileType::RawS16 ? 4 : 2;
        const uint32_t bytes_per_second = sampling_rate * bytes_per_sample;
        const uint32_t available_seconds = space_info.free / bytes_per_second;
        const uint32_t seconds = available_seconds % 60;
        const uint32_t available_minutes = available_seconds / 60;
        const uint32_t minutes = available_minutes % 60;
        const uint32_t hours = available_minutes / 60;
        const std::string available_time =
            to_string_dec_uint(hours, 3, ' ') + ":" +
            to_string_dec_uint(minutes, 2, '0') + ":" +
            to_string_dec_uint(seconds, 2, '0');
        text_time_available.set(available_time);
    }
}

void RecordView::handle_capture_thread_done(const File::Error error) {
    stop();
    if (error.code()) {
        handle_error(error);
    }
}

void RecordView::handle_error(const File::Error error) {
    if (on_error) {
        on_error(error.what());
    }
}

} /* namespace ui */
