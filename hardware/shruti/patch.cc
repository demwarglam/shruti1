// Copyright 2009 Olivier Gillet. All rights reserved
//
// Author: Olivier Gillet (ol.gillet@gmail.com)
//
// Patch.

#include "hardware/shruti/patch.h"

#include <avr/eeprom.h>
#include <avr/pgmspace.h>

#include "hardware/io/serial.h"
#include "hardware/shruti/display.h"
#include "hardware/utils/op.h"

using namespace hardware_io;
using hardware_utils::Op;

namespace hardware_shruti {

void Patch::Pack(uint8_t* patch_buffer) const {
  for (uint8_t i = 0; i < 28; ++i) {
    patch_buffer[i] = osc_algorithm[i];
  }
  for (uint8_t i = 0; i < kSavedModulationMatrixSize; ++i) {
    patch_buffer[2 * i + 28] = modulation_matrix.modulation[i].source |
        Op::ShiftLeft4(modulation_matrix.modulation[i].destination);
    patch_buffer[2 * i + 29] = modulation_matrix.modulation[i].amount;
  }
  for (uint8_t i = 0; i < 8; ++i) {
    patch_buffer[2 * kSavedModulationMatrixSize + 28 + i] = sequence[i];
  }
  for (uint8_t i = 0; i < kPatchNameSize; ++i) {
    patch_buffer[2 * kSavedModulationMatrixSize + 28 + 8 + i] = name[i];
  }
}

uint8_t Patch::CheckBuffer() {
  for (uint8_t i = 6; i < 26; ++i) {
    if (load_save_buffer_[i] > 128) {
      return 0;
    }
  }
  const uint8_t name_offset = 2 * kSavedModulationMatrixSize + 28 + 8;
  for (uint8_t i = name_offset; i < name_offset + kPatchNameSize; ++i) {
    if (load_save_buffer_[i] > 128) {
      return 0;
    }
  }
  return 1;
}

void Patch::Unpack(const uint8_t* patch_buffer) {
  for (uint8_t i = 0; i < 28; ++i) {
    osc_algorithm[i] = patch_buffer[i];
  }
  for (uint8_t i = 0; i < kSavedModulationMatrixSize; ++i) {
    modulation_matrix.modulation[i].source = patch_buffer[2 * i + 28] & 0xf;
    modulation_matrix.modulation[i].destination = Op::ShiftRight4(
        patch_buffer[2 * i + 28]);
    modulation_matrix.modulation[i].amount = patch_buffer[2 * i + 29];
  }
  for (uint8_t i = 0; i < 8; ++i) {
    sequence[i] = patch_buffer[i + 2 * kSavedModulationMatrixSize + 28];
  }
  for (uint8_t i = 0; i < 8; ++i) {
    name[i] = patch_buffer[i + 2 * kSavedModulationMatrixSize + 28 + 8];
  }
}

void Patch::EepromSave(uint8_t slot) const {
  Pack(load_save_buffer_);
  int16_t offset = slot * kSerializedPatchSize;
  for (int16_t i = 0; i < kSerializedPatchSize; ++i) {
    eeprom_write_byte((uint8_t*)(i + offset), load_save_buffer_[i]);
  }
}

void Patch::EepromLoad(uint8_t slot) {
  int16_t offset = slot * kSerializedPatchSize;
  for (int16_t i = 0; i < kSerializedPatchSize; ++i) {
    load_save_buffer_[i] = eeprom_read_byte((uint8_t*)(i + offset));
  }
  if (CheckBuffer()) {
    Unpack(load_save_buffer_);
  } else {
    name[0] = '?';
  }
}

static const prog_char sysex_header[] PROGMEM = {
  0xf0,  // <SysEx>
  0x00, 0x20, 0x77,  // TODO(pichenettes): register manufacturer ID.
  0x00, 0x01,  // Product ID for Shruti-1.
  0x01,  // Command: patch transfer.
  0x00,  // Argument: none.
};

void Patch::SysExSend() const {
  Serial<SerialPort0, 31250, DISABLED, POLLED> midi_output;
  
  Pack(load_save_buffer_);
  
  // Outputs the SysEx header.
  for (uint8_t i = 0; i < sizeof(sysex_header); ++i) {
    midi_output.Write(pgm_read_byte(sysex_header + i));
  }
  
  // Outputs the patch data, in high-low nibblized form.
  uint8_t checksum = 0;  // Sum of all patch data bytes.
  for (uint8_t i = 0; i < kSerializedPatchSize; ++i) {
    checksum += load_save_buffer_[i];
    midi_output.Write(Op::ShiftRight4(load_save_buffer_[i]));
    midi_output.Write(load_save_buffer_[i] & 0x0f);
  }
  
  midi_output.Write(Op::ShiftRight4(checksum));
  midi_output.Write(checksum & 0x0f);
  
  midi_output.Write(0xf7);  // </SysEx>
}

void Patch::SysExReceive(uint8_t sysex_byte) {
  if (sysex_byte == 0xf0) {
    sysex_reception_checksum_ = 0;
    sysex_bytes_received_ = 0;
    sysex_reception_state_ = RECEIVING_HEADER;
  }
  switch (sysex_reception_state_) {
    case RECEIVING_HEADER:
      if (pgm_read_byte(sysex_header + sysex_bytes_received_) == sysex_byte) {
        sysex_bytes_received_++;
        if (sysex_bytes_received_ >= sizeof(sysex_header)) {
          sysex_reception_state_ = RECEIVING_DATA;
          sysex_bytes_received_ = 0;
        }
      } else {
        sysex_reception_state_ = RECEIVING_FOOTER;
      }
      break;
  case RECEIVING_DATA:
    {
      uint8_t i = sysex_bytes_received_ >> 1;
      if (sysex_bytes_received_ & 1) {
        load_save_buffer_[i] |= sysex_byte & 0xf;
        if (i < kSerializedPatchSize) {
          sysex_reception_checksum_ += load_save_buffer_[i];
        }
      } else {
          load_save_buffer_[i] = Op::ShiftLeft4(sysex_byte);
      }
      sysex_bytes_received_++;
      if (sysex_bytes_received_ >= (kSerializedPatchSize + 1) * 2) {
        sysex_reception_state_ = RECEIVING_FOOTER;
      }
    }
    break;
  case RECEIVING_FOOTER:
    if (sysex_byte == 0xf7 &&
        sysex_reception_checksum_ == load_save_buffer_[kSerializedPatchSize] &&
        CheckBuffer()) {
      Unpack(load_save_buffer_);
      sysex_reception_state_ = RECEPTION_OK;
    } else {
      sysex_reception_state_ = RECEPTION_ERROR;
    }
    break;
  }
}

void Patch::Backup() const {
  Pack(undo_buffer_);
}

void Patch::Restore() {
  Unpack(undo_buffer_);
}

/* static */
uint8_t Patch::load_save_buffer_[kSerializedPatchSize + 1];

/* static */
uint8_t Patch::undo_buffer_[kSerializedPatchSize];

/* static */
uint8_t Patch::sysex_bytes_received_;

/* static */
uint8_t Patch::sysex_reception_checksum_;

/* static */
uint8_t Patch::sysex_reception_state_;

}  // hardware_shruti
