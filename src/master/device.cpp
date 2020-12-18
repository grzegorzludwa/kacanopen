/*
 * Copyright (c) 2015-2016, Thomas Keh
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *
 *    3. Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived from
 *       this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "kacanopen/master/device.h"
#include "kacanopen/core/core.h"
#include "kacanopen/core/global_config.h"
#include "kacanopen/core/logger.h"
#include "kacanopen/core/sdo_error.h"
#include "kacanopen/master/dictionary_error.h"
#include "kacanopen/master/profiles.h"
#include "kacanopen/master/utils.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <memory>

namespace kaco {

Device::Device(Core& core, uint8_t node_id)
    : m_core(core),
      m_node_id(node_id),
      m_eds_library(m_dictionary, m_name_to_address),
      terminating_(false) {}

Device::~Device() {
  for (auto& cob_id : cob_ids_) m_core.pdo.remove_pdo_received_callback(cob_id);
  stop_request_heartbeat();
}

void Device::start() {
  load_operations();
  load_constants();

  // NOTE: Loading these files SOMETIMES causes a inbalance between m_dictionary
  // and m_name_to_address which causes segfaults when parsing messages at
  // runtime

  // load_default_eds_files();

  m_core.nmt.send_nmt_message(m_node_id, NMT::Command::start_node);
}

uint8_t Device::get_node_id() const { return m_node_id; }

bool Device::has_entry(const std::string& entry_name) {
  const std::string name = Utils::escape(entry_name);
  return m_name_to_address.count(name) > 0 &&
         has_entry(m_name_to_address[name].index,
                   m_name_to_address[name].subindex);
}

bool Device::has_entry(const uint16_t index, const uint8_t subindex) {
  return m_dictionary.count(Address{index, subindex}) > 0;
}

Type Device::get_entry_type(const std::string& entry_name) {
  const std::string name = Utils::escape(entry_name);
  if (!has_entry(name)) {
    throw dictionary_error(dictionary_error::type::unknown_entry, name);
  }
  return m_dictionary[m_name_to_address[name]].get_type();
}

Type Device::get_entry_type(const uint16_t index, const uint8_t subindex) {
  if (!has_entry(index, subindex)) {
    throw dictionary_error(
        dictionary_error::type::unknown_entry,
        std::to_string(index) + "sub" + std::to_string(subindex));
  }
  return m_dictionary[Address{index, subindex}].get_type();
}

const Value& Device::get_entry(const std::string& entry_name,
                               const ReadAccessMethod access_method) {
  const std::string name = Utils::escape(entry_name);
  if (!has_entry(name)) {
    throw dictionary_error(dictionary_error::type::unknown_entry, name);
  }
  const Address address = m_name_to_address[name];
  return get_entry(address.index, address.subindex, access_method);
}

const Value& Device::get_entry(const uint16_t index, const uint8_t subindex,
                               const ReadAccessMethod access_method) {
  if (!has_entry(index, subindex)) {
    throw dictionary_error(
        dictionary_error::type::unknown_entry,
        std::to_string(index) + "sub" + std::to_string(subindex));
  }
  Entry& entry = m_dictionary[Address{index, subindex}];
  if (access_method == ReadAccessMethod::sdo ||
      (access_method == ReadAccessMethod::use_default &&
       entry.read_access_method == ReadAccessMethod::sdo)) {
    DEBUG_LOG("[Device::get_entry] SDO update on read.");
    entry.set_value(get_entry_via_sdo(entry.index, entry.subindex, entry.type));
  }
  // NOTE: If not, it has to be a PDO. The PDO caching or waiting is not
  // implemented, so the entry can be Invalid at init time. We force then an
  // update through SDO
  //  else {
  //    if (entry.get_value().type == kaco::Type::invalid) {
  //      entry.set_value(
  //          get_entry_via_sdo(entry.index, entry.subindex, entry.type));
  //    }
  //  }
  return entry.get_value();
}

void Device::set_entry(const std::string& entry_name, const Value& value,
                       const WriteAccessMethod access_method) {
  const std::string name = Utils::escape(entry_name);
  if (!has_entry(name)) {
    throw dictionary_error(dictionary_error::type::unknown_entry, name);
  }
  const Address address = m_name_to_address[name];
  return set_entry(address.index, address.subindex, value, access_method);
}

void Device::set_entry(const uint16_t index, const uint8_t subindex,
                       const Value& value,
                       const WriteAccessMethod access_method) {
  const std::string index_string =
      std::to_string(index) + "sub" + std::to_string(subindex);
  if (!has_entry(index, subindex)) {
    throw dictionary_error(dictionary_error::type::unknown_entry, index_string);
  }
  Entry& entry = m_dictionary[Address{index, subindex}];
  if (value.type != entry.type) {
    throw dictionary_error(
        dictionary_error::type::wrong_type, index_string,
        "Entry type: " + Utils::type_to_string(entry.type) +
            ", given type: " + Utils::type_to_string(value.type));
  }
  entry.set_value(value);
  if (access_method == WriteAccessMethod::sdo ||
      (access_method == WriteAccessMethod::use_default &&
       entry.write_access_method == WriteAccessMethod::sdo)) {
    DEBUG_LOG("[Device::set_entry] SDO update on write.");
    set_entry_via_sdo(entry.index, entry.subindex, value);
  }
}

void Device::add_entry(const uint16_t index, const uint8_t subindex,
                       const std::string& name, const Type type,
                       const AccessType access_type) {
  const std::string entry_name = Utils::escape(name);
  if (m_name_to_address.count(entry_name) > 0) {
    throw canopen_error("[Device::add_entry] Entry with name \"" + entry_name +
                        "\" already exists.");
  }
  if (has_entry(index, subindex)) {
    throw canopen_error("[Device::add_entry] Entry with index " +
                        std::to_string(index) + "sub" +
                        std::to_string(subindex) + " already exists.");
  }
  Entry entry(index, subindex, entry_name, type, access_type);
  const Address address{index, subindex};
  m_dictionary.insert(std::make_pair(address, std::move(entry)));
  m_name_to_address.insert(std::make_pair(entry_name, address));
}

void Device::add_receive_pdo_mapping(uint16_t cob_id,
                                     const std::string& entry_name,
                                     uint8_t offset) {
  // TODO: update entry's default access method

  const std::string name = Utils::escape(entry_name);

  if (!has_entry(name)) {
    throw dictionary_error(dictionary_error::type::unknown_entry, name);
  }

  Entry& entry = m_dictionary[m_name_to_address[name]];

  const uint8_t type_size = Utils::get_type_size(entry.type);

  if (offset + type_size > 8) {
    throw dictionary_error(dictionary_error::type::mapping_size, name,
                           "offset (" + std::to_string(offset) +
                               ") + type_size (" + std::to_string(type_size) +
                               ") > 8.");
  }

  ReceivePDOMapping* pdo_temp;

  {
    std::lock_guard<std::mutex> lock(m_receive_pdo_mappings_mutex);
    m_receive_pdo_mappings.push_front({cob_id, name, offset});
    pdo_temp = &m_receive_pdo_mappings.front();
  }

  ReceivePDOMapping& pdo = *pdo_temp;

  // TODO: this only works while add_pdo_received_callback takes callback by
  // value.
  auto binding = std::bind(&Device::pdo_received_callback, this, pdo,
                           std::placeholders::_1);
  cob_ids_.push_back(cob_id);
  m_core.pdo.add_pdo_received_callback(cob_id, std::move(binding));
}

void Device::add_receive_pdo_mapping(
    uint16_t cob_id, const std::string& entry_name, uint8_t offset,
    std::function<void(const ReceivePDOMapping&, std::vector<uint8_t>)>
        funtion) {
  // TODO: update entry's default access method

  const std::string name = Utils::escape(entry_name);

  if (!has_entry(name)) {
    throw dictionary_error(dictionary_error::type::unknown_entry, name);
  }

  Entry& entry = m_dictionary[m_name_to_address[name]];

  const uint8_t type_size = Utils::get_type_size(entry.type);

  if (offset + type_size > 8) {
    throw dictionary_error(dictionary_error::type::mapping_size, name,
                           "offset (" + std::to_string(offset) +
                               ") + type_size (" + std::to_string(type_size) +
                               ") > 8.");
  }

  ReceivePDOMapping* pdo_temp;

  {
    std::lock_guard<std::mutex> lock(m_receive_pdo_mappings_mutex);
    m_receive_pdo_mappings.push_front({cob_id, name, offset});
    pdo_temp = &m_receive_pdo_mappings.front();
  }

  ReceivePDOMapping& pdo = *pdo_temp;

  // TODO: this only works while add_pdo_received_callback takes callback by
  // value.
  auto binding = std::bind(funtion, pdo, std::placeholders::_1);
  m_core.pdo.add_pdo_received_callback(cob_id, std::move(binding));
}

void Device::add_receive_pdo_mapping(uint16_t cob_id, uint16_t entry_index,
                                     uint8_t entry_subindex, uint8_t offset) {
  Address entry_addresss_temp{entry_index, entry_subindex};
  Address& entry_addresss = entry_addresss_temp;
  Entry& entry = m_dictionary[entry_addresss];
  const uint8_t type_size = Utils::get_type_size(entry.type);

  if (offset + type_size > 8) {
    throw dictionary_error(dictionary_error::type::mapping_size, entry.name,
                           "offset (" + std::to_string(offset) +
                               ") + type_size (" + std::to_string(type_size) +
                               ") > 8.");
  }

  ReceivePDOMapping* pdo_temp;

  {
    std::lock_guard<std::mutex> lock(m_receive_pdo_mappings_mutex);
    m_receive_pdo_mappings.push_front({cob_id, entry.name, offset});
    pdo_temp = &m_receive_pdo_mappings.front();
  }

  ReceivePDOMapping& pdo = *pdo_temp;
  auto binding = std::bind(&Device::pdo_received_callback, this, pdo,
                           std::placeholders::_1);
  cob_ids_.push_back(cob_id);
  m_core.pdo.add_pdo_received_callback(cob_id, std::move(binding));
}

void Device::add_transmit_pdo_mapping(uint16_t cob_id,
                                      const std::vector<Mapping>& mappings,
                                      TransmissionType transmission_type,
                                      std::chrono::milliseconds repeat_time) {
  TransmitPDOMapping* pdo_temp;

  {
    std::lock_guard<std::mutex> lock(
        m_transmit_pdo_mappings_mutex);  // unlocks in case of exception
    // Contructor can throw dictionary_error. Letting user handle this.
    m_transmit_pdo_mappings.emplace_front(
        m_core, m_dictionary, m_name_to_address, cob_id, transmission_type,
        repeat_time, mappings);
    pdo_temp = &m_transmit_pdo_mappings.front();
  }

  TransmitPDOMapping& pdo = *pdo_temp;

  if (transmission_type == TransmissionType::ON_CHANGE) {
    for (const Mapping& mapping : pdo.mappings) {
      const std::string entry_name = Utils::escape(mapping.entry_name);

      // entry exists because check_correctness() == true.
      Entry& entry = m_dictionary.at(m_name_to_address.at(entry_name));

      entry.add_value_changed_callback([entry_name, &pdo](const Value& value) {
        DEBUG_LOG("[Callback] Value of " << entry_name << " changed to "
                                         << value);
        pdo.send();
      });
    }

  } else {
    // transmission_type==TransmissionType::PERIODIC

    if (repeat_time == std::chrono::milliseconds(0)) {
      WARN(
          "[Device::add_transmit_pdo_mapping] Repeat time is 0. This could "
          "overload the bus.");
    }

    pdo.run_periodic_transmitter = true;
    pdo.periodic_transmitter =
        std::unique_ptr<std::thread>(new std::thread([&pdo, repeat_time]() {
          while (pdo.run_periodic_transmitter) {
            DEBUG_LOG("[Timer thread] Sending periodic PDO.");
            pdo.send();
            std::this_thread::sleep_for(repeat_time);
          }
        }));
  }
}

void Device::add_transmit_pdo_mapping(
    uint16_t cob_id, const std::vector<MappingByIndex>& mappings_by_index,
    TransmissionType transmission_type, std::chrono::milliseconds repeat_time) {
  TransmitPDOMapping* pdo_temp;

  // Wrap MappingByIndex to Mapping
  std::vector<Mapping> mappings;

  for (auto i : mappings_by_index) {
    Address entry_addresss_temp{i.entry_index, i.entry_subindex};
    Address& entry_addresss = entry_addresss_temp;
    Entry& entry = m_dictionary[entry_addresss];
    Mapping mapping_entry_temp;
    mapping_entry_temp.entry_name = entry.name;
    mapping_entry_temp.offset = i.offset;
    mappings.push_back(mapping_entry_temp);
  }

  {
    std::lock_guard<std::mutex> lock(
        m_transmit_pdo_mappings_mutex);  // unlocks in case of exception
    // Contructor can throw dictionary_error. Letting user handle this.
    m_transmit_pdo_mappings.emplace_front(
        m_core, m_dictionary, m_name_to_address, cob_id, transmission_type,
        repeat_time, mappings);
    pdo_temp = &m_transmit_pdo_mappings.front();
  }

  TransmitPDOMapping& pdo = *pdo_temp;

  if (transmission_type == TransmissionType::ON_CHANGE) {
    for (const Mapping& mapping : pdo.mappings) {
      const std::string entry_name = Utils::escape(mapping.entry_name);

      // entry exists because check_correctness() == true.
      Entry& entry = m_dictionary.at(m_name_to_address.at(entry_name));

      entry.add_value_changed_callback([entry_name, &pdo](const Value& value) {
        DEBUG_LOG("[Callback] Value of " << entry_name << " changed to "
                                         << value);
        pdo.send();
      });
    }

  } else {
    // transmission_type==TransmissionType::PERIODIC

    if (repeat_time == std::chrono::milliseconds(0)) {
      WARN(
          "[Device::add_transmit_pdo_mapping] Repeat time is 0. This could "
          "overload the bus.");
    }

    pdo.run_periodic_transmitter = true;
    pdo.periodic_transmitter =
        std::unique_ptr<std::thread>(new std::thread([&pdo, repeat_time]() {
          while (pdo.run_periodic_transmitter) {
            DEBUG_LOG("[Timer thread] Sending periodic PDO.");
            pdo.send();
            std::this_thread::sleep_for(repeat_time);
          }
        }));
  }
}

void Device::pdo_received_callback(const ReceivePDOMapping& mapping,
                                   std::vector<uint8_t> data) {
  DEBUG_LOG("[Device::pdo_received_callback] Received a PDO for mapping '"
            << mapping.entry_name << "'!");

  const std::string entry_name = Utils::escape(mapping.entry_name);
  Entry& entry = m_dictionary[m_name_to_address[entry_name]];
  const uint8_t offset = mapping.offset;
  const uint8_t type_size = Utils::get_type_size(entry.type);

  if (entry.type == Type::invalid) {
    ERROR("[Device::pdo_received_callback] Entry '" + entry_name +
          "' fetched from m_dicctionary is invalid");
    return;
  }

  if (data.size() < offset + type_size) {
    // We don't throw an exception here, because this could be a network error.
    WARN("[Device::pdo_received_callback] PDO has wrong size. Ignoring it...");
    DUMP(data.size());
    DUMP(offset);
    DUMP(type_size);
    return;
  }

  DEBUG_LOG("Updating entry " << entry.name << ".");
  // std::vector<uint8_t> bytes(data.begin()+offset,
  // data.begin()+offset+type_size);
  std::vector<uint8_t> bytes(data.begin() + offset,
                             data.begin() + offset + type_size);
  entry.set_value(Value(entry.type, bytes));
}

uint16_t Device::get_device_profile_number() {
  // Using the address here because this makes read_dictionary_from_eds()
  // shorter...
  uint32_t device_type = get_entry(0x1000);  // Device type
  return (device_type & 0xFFFF);
}

Value Device::get_entry_via_sdo(uint32_t index, uint8_t subindex, Type type) {
  sdo_error last_error(sdo_error::type::unknown);

  for (size_t i = 0; i < Config::repeats_on_sdo_timeout + 1; ++i) {
    try {
      std::vector<uint8_t> data = m_core.sdo.upload(m_node_id, index, subindex);
      return Value(type, data);
    } catch (const sdo_error& error) {
      last_error = error;
      if (i < Config::repeats_on_sdo_timeout) {
        DEBUG_LOG("[Device::get_entry_via_sdo] device "
                  << std::to_string(m_node_id) << " " << error.what()
                  << " -> Repetition " << std::to_string(i + 1) << " of "
                  << std::to_string(Config::repeats_on_sdo_timeout + 1) << ".");
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kaco::Config::sdo_response_timeout_ms));
      }
    }
  }

  throw sdo_error(
      sdo_error::type::response_timeout,
      "Device::get_entry_via_sdo() device " + std::to_string(m_node_id) +
          " failed after " +
          std::to_string(Config::repeats_on_sdo_timeout + 1) +
          " repeats. Last error: " + std::string(last_error.what()));
}

void Device::set_entry_via_sdo(uint32_t index, uint8_t subindex,
                               const Value& value) {
  sdo_error last_error(sdo_error::type::unknown);

  for (size_t i = 0; i < Config::repeats_on_sdo_timeout + 1; ++i) {
    try {
      const auto& bytes = value.get_bytes();
      m_core.sdo.download(m_node_id, index, subindex, bytes.size(), bytes);
      return;
    } catch (const sdo_error& error) {
      last_error = error;
      if (i < Config::repeats_on_sdo_timeout) {
        DEBUG_LOG("[Device::set_entry_via_sdo] device "
                  << std::to_string(m_node_id) << " " << error.what()
                  << " -> Repetition " << std::to_string(i + 1) << " of "
                  << std::to_string(Config::repeats_on_sdo_timeout + 1) << ".");
        std::this_thread::sleep_for(
            std::chrono::milliseconds(kaco::Config::sdo_response_timeout_ms));
      }
    }
  }

  throw sdo_error(
      sdo_error::type::response_timeout,
      "Device::set_entry_via_sdo() device " + std::to_string(m_node_id) +
          " failed after " +
          std::to_string(Config::repeats_on_sdo_timeout + 1) +
          " repeats. Last error: " + std::string(last_error.what()));
}

std::string Device::load_dictionary_from_library() {
  if (!m_eds_library.ready()) {
    throw canopen_error(
        "[Device::load_dictionary_from_library] EDS library is not available.");
  }

  DEBUG_LOG("Device::load_dictionary_from_library()...");
  std::string eds_path = "";

  // First, we try to load manufacturer specific entries.

  Config::eds_library_clear_dictionary = true;
  const bool success = m_eds_library.load_manufacturer_eds(*this);
  Config::eds_library_clear_dictionary = false;

  if (success) {
    DEBUG_LOG("[Device::load_dictionary_from_library] Device "
              << std::to_string(m_node_id)
              << ": Successfully loaded manufacturer-specific dictionary: "
              << m_eds_library.get_most_recent_eds_file_path());
    DEBUG_LOG(
        "[Device::load_dictionary_from_library] Now we will add additional "
        "mappings from standard conformal entry names to the entries...");
    eds_path = m_eds_library.get_most_recent_eds_file_path();
    Config::eds_reader_just_add_mappings = true;
  } else {
    DEBUG_LOG("[Device::load_dictionary_from_library] Device "
              << std::to_string(m_node_id)
              << ": There is no manufacturer-specific EDS file available. "
                 "Going on with the default dictionary...");
    Config::eds_reader_just_add_mappings = false;  // should be already false...
  }

  // Load entries like they are defined in the CiA CANopen standard documents...
  // Either just the names are added or the whole dictionary depending on
  // Config::eds_reader_just_add_mappings
  load_cia_dictionary();
  if (eds_path.empty()) {
    // no manufacturer EDS...
    eds_path = m_eds_library.get_most_recent_eds_file_path();
  }
  Config::eds_reader_just_add_mappings = false;

  return eds_path;
}

void Device::load_cia_dictionary() {
  Config::eds_reader_mark_entries_as_generic = true;
  const uint16_t profile = get_device_profile_number();
  if (m_eds_library.load_default_eds(profile)) {
    DEBUG_LOG("[Device::load_dictionary_from_library] Device "
              << std::to_string(m_node_id)
              << ": Successfully loaded profile-specific dictionary: "
              << m_eds_library.get_most_recent_eds_file_path());
  } else {
    Config::eds_library_clear_dictionary = false;  // should be already false...
    if (m_eds_library.load_mandatory_entries()) {
      DEBUG_LOG("[Device::load_dictionary_from_library] Device "
                << std::to_string(m_node_id)
                << ": Successfully loaded mandatory entries: "
                << m_eds_library.get_most_recent_eds_file_path());
    } else {
      throw canopen_error(
          "Could not load mandatory CiA 301 dictionary entries for device with "
          "ID " +
          std::to_string(m_node_id) +
          ". This can break various parts of KaCanOpen!");
    }
  }
  Config::eds_reader_mark_entries_as_generic = false;
}

void Device::load_dictionary_from_eds(const std::string& path) {
  m_eds_library.reset_dictionary();
  Config::eds_reader_just_add_mappings = false;  // should be already false...
  Config::eds_reader_mark_entries_as_generic =
      false;  // should be already false...
  EDSReader reader(m_dictionary, m_name_to_address);

  if (!reader.load_file(path)) {
    throw canopen_error(
        "[EDSLibrary::load_dictionary_from_eds] Loading file not successful: " +
        path);
  }

  if (!reader.import_entries()) {
    throw canopen_error(
        "[EDSLibrary::load_dictionary_from_eds] Importing entries failed for "
        "file " +
        path);
  }

  // Load generic names from the standard CiA profiles on top of the existing
  // dictionary.
  if (m_eds_library.ready()) {
    // Wo know nothing about the EDS... No mandatory entries here. At least
    // 0x1000 is required for load_cia_dictionary():
    if (!has_entry(0x1000)) {
      add_entry(0x1000, 0, "device_type", Type::uint32, AccessType::read_only);
    }
    Config::eds_reader_just_add_mappings = true;
    load_cia_dictionary();
    Config::eds_reader_just_add_mappings = false;
  } else {
    WARN(
        "[Device::load_dictionary_from_eds] Cannot load generic entry names "
        "because EDS library is not available.");
  }
}

void Device::load_default_eds_files() {
  if (!m_eds_library.lookup_library()) {
    throw canopen_error(
        "[Device::start] EDS library not found. If and only if you make sure "
        "for yourself, that mandatory"
        " entries and operations are available, you can catch this error and "
        "go on.");
  }

  if (!m_eds_library.load_mandatory_entries()) {
    throw canopen_error(
        "[Device::start] Could not load mandatory dictionary entries."
        " If and only if you make sure for yourself, that mandatory"
        " entries and operations are available, you can catch this error and "
        "go on.");
  }
}

bool Device::load_operations() {
  const uint16_t profile = get_device_profile_number();
  if (Profiles::operations.count(profile) > 0) {
    m_operations.insert(Profiles::operations.at(profile).cbegin(),
                        Profiles::operations.at(profile).cend());
    return true;
  }
  return false;
}

void Device::add_operation(const std::string& operation_name,
                           const Operation& operation) {
  const std::string name = Utils::escape(operation_name);
  if (m_operations.count(name) > 0) {
    WARN("[Device::add_operation] Overwriting operation \"" << name << "\".");
  }
  m_operations[name] = operation;
}

Value Device::execute(const std::string& operation_name,
                      const Value& argument) {
  const std::string name = Utils::escape(operation_name);
  if (m_operations.count(name) == 0) {
    throw dictionary_error(dictionary_error::type::unknown_operation, name);
  }
  return m_operations[name](*this, argument);
}

bool Device::load_constants() {
  const uint16_t profile = get_device_profile_number();
  if (Profiles::constants.count(profile) > 0) {
    m_constants.insert(Profiles::constants.at(profile).cbegin(),
                       Profiles::constants.at(profile).cend());
    return true;
  }
  return false;
}

void Device::add_constant(const std::string& constant_name,
                          const Value& constant) {
  const std::string name = Utils::escape(constant_name);
  if (m_constants.count(name) > 0) {
    WARN("[Device::add_constant] Overwriting constant \"" << name << "\".");
  }
  m_constants[name] = constant;
}

const Value& Device::get_constant(const std::string& constant_name) const {
  const std::string name = Utils::escape(constant_name);
  if (m_constants.count(name) == 0) {
    throw dictionary_error(dictionary_error::type::unknown_constant, name);
  }
  return m_constants.at(name);
}

void Device::print_dictionary() const {
  using EntryRef = std::reference_wrapper<const kaco::Entry>;
  std::vector<EntryRef> entries;

  for (const auto& pair : m_dictionary) {
    if (!pair.second.disabled) {
      entries.push_back(std::ref(pair.second));
    }
  }

  // sort by index and subindex
  std::sort(
      entries.begin(), entries.end(),
      [](const EntryRef& l, const EntryRef& r) { return l.get() < r.get(); });

  for (const auto& entry : entries) {
    entry.get().print();
  }
}

void Device::read_complete_dictionary() {
  for (auto& pair : m_dictionary) {
    try {
      get_entry(pair.second.name);
    } catch (const sdo_error& error) {
      pair.second.disabled = true;
      DEBUG_LOG("[Device::read_complete_dictionary] SDO error for field "
                << pair.second.name << ": " << error.what()
                << " -> disable entry.");
    }
  }
}

const Value Device::m_dummy_value = Value();

void Device::send_heartbeat(uint8_t node_id, uint16_t heartbeat_interval,
                            bool rtr, NMT::State state) {
  const kaco::Message request_heartbeat = {
      static_cast<uint16_t>(0x700 + node_id),
      rtr,
      0x01,
      {static_cast<uint8_t>(state)}};
  while (!terminating_) {
    m_core.send(request_heartbeat);
    std::this_thread::sleep_for(std::chrono::milliseconds(heartbeat_interval));
  }
}

void Device::request_heartbeat(uint8_t node_id, uint16_t heartbeat_interval,
                               bool rtr, NMT::State state) {
  if (heartbeat_interval) {
    if (!request_heartbeat_thread_) {
      request_heartbeat_thread_.reset(
          new std::thread(&Device::send_heartbeat, this, node_id,
                          heartbeat_interval, rtr, state));
    }
  }
}

void Device::stop_request_heartbeat() {
  terminating_ = true;
  if (request_heartbeat_thread_) {
    if (request_heartbeat_thread_->joinable())
      request_heartbeat_thread_->join();
  }
}

void Device::send_consumer_heartbeat(uint8_t node_id,
                                     uint16_t heartbeat_interval, bool rtr,
                                     NMT::State state) {
  request_heartbeat(node_id, heartbeat_interval, rtr, state);
}

void Device::stop_send_consumer_heartbeat() { stop_request_heartbeat(); }

std::pair<uint16_t, uint16_t> Device::get_tpdo_indexes(kaco::TPDO_NO tpdo_no) {
  uint16_t comm_param_idx;
  uint16_t mapp_param_idx;

  switch (tpdo_no)
  {
  case kaco::TPDO_NO::TPDO_1:
    comm_param_idx = 0x1800;
    mapp_param_idx = 0x1A00;
    break;
  case kaco::TPDO_NO::TPDO_2:
    comm_param_idx = 0x1801;
    mapp_param_idx = 0x1A01;
    break;
  case kaco::TPDO_NO::TPDO_3:
    comm_param_idx = 0x1802;
    mapp_param_idx = 0x1A02;
    break;
  case kaco::TPDO_NO::TPDO_4:
    comm_param_idx = 0x1803;
    mapp_param_idx = 0x1A03;
    break;
  default:
    throw canopen_error(
        "[Device::get_tpdo_indexes] Invalide pdo_no");
  }

  return {comm_param_idx, mapp_param_idx};
}

std::pair<uint16_t, uint16_t> Device::get_rpdo_indexes(kaco::RPDO_NO rpdo_no) {
  uint16_t comm_param_idx;
  uint16_t mapp_param_idx;

  switch (rpdo_no)
  {
  case kaco::RPDO_NO::RPDO_1:
    comm_param_idx = 0x1400;
    mapp_param_idx = 0x1600;
    break;
  case kaco::RPDO_NO::RPDO_2:
    comm_param_idx = 0x1401;
    mapp_param_idx = 0x1601;
    break;
  case kaco::RPDO_NO::RPDO_3:
    comm_param_idx = 0x1402;
    mapp_param_idx = 0x1602;
    break;
  case kaco::RPDO_NO::RPDO_4:
    comm_param_idx = 0x1403;
    mapp_param_idx = 0x1603;
    break;
  default:
    throw canopen_error(
        "[Device::get_tpdo_indexes] Invalide pdo_no");
  }

  return {comm_param_idx, mapp_param_idx};
}

void Device::write_entry(uint16_t index, std::vector<uint32_t> entries) {
  uint8_t offset = 0;
  for (uint8_t i = 0; i < entries.size(); i++) {
    offset++;
    set_entry(index, offset, static_cast<uint32_t>(entries.at(i)),
                      kaco::WriteAccessMethod::sdo);
  }
}

void Device::map_tpdo_in_device(kaco::TPDO_NO tpdo_no,
                        std::vector<uint32_t> entries_to_be_mapped,
                        uint8_t transmit_type, uint16_t inhibit_time,
                        uint16_t event_timer)
{
  auto tmp_idxs = get_tpdo_indexes(tpdo_no);

  uint16_t comm_param_idx = tmp_idxs.first;
  uint16_t mapp_param_idx = tmp_idxs.second;

  // disable tpdo
  uint32_t cob_id = 0;
  cob_id = get_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                             kaco::ReadAccessMethod::sdo);
  cob_id ^= static_cast<uint32_t>((-1 ^ cob_id) & (1UL << 31));
  set_entry(comm_param_idx, 0x01, static_cast<uint32_t>(cob_id),
                    kaco::WriteAccessMethod::sdo);

  // delete no. of mapped entries
  set_entry(mapp_param_idx, 0x00, static_cast<uint8_t>(0x00),
                    kaco::WriteAccessMethod::sdo);
                    
  // add new mapping
  write_entry(mapp_param_idx, entries_to_be_mapped);

  // update no. of mapped entries
  set_entry(mapp_param_idx, static_cast<uint8_t>(0x00),
                    static_cast<uint8_t>(entries_to_be_mapped.size()),
                    kaco::WriteAccessMethod::sdo);

  // set transmit type
  set_entry(comm_param_idx, static_cast<uint8_t>(0x02), transmit_type,
                    kaco::WriteAccessMethod::sdo);

  // set inhibit time
  set_entry(comm_param_idx, static_cast<uint8_t>(0x03), inhibit_time,
                    kaco::WriteAccessMethod::sdo);
                    
  // set event timer i.e transmit frequency
  set_entry(comm_param_idx, static_cast<uint8_t>(0x05), event_timer,
                    kaco::WriteAccessMethod::sdo);

  // enable tpdo
  cob_id ^= static_cast<uint32_t>((0 ^ cob_id) & (1UL << 31));
  set_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                    static_cast<uint32_t>(cob_id),
                    kaco::WriteAccessMethod::sdo);
}

void Device::map_tpdo_in_device(kaco::TPDO_NO tpdo_no,
                        std::vector<uint32_t> entries_to_be_mapped,
                        uint8_t transmit_type, uint16_t inhibit_time) {
  auto tmp_idxs = get_tpdo_indexes(tpdo_no);

  uint16_t comm_param_idx = tmp_idxs.first;
  uint16_t mapp_param_idx = tmp_idxs.second;

  // disable tpdo
  uint32_t cob_id = 0;
  cob_id = get_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                             kaco::ReadAccessMethod::sdo);
  cob_id ^= static_cast<uint32_t>((-1 ^ cob_id) & (1UL << 31));
  set_entry(comm_param_idx, 0x01, static_cast<uint32_t>(cob_id),
                    kaco::WriteAccessMethod::sdo);

  // delete no. of mapped entries
  set_entry(mapp_param_idx, 0x00, static_cast<uint8_t>(0x00),
                    kaco::WriteAccessMethod::sdo);
                    
  // add new mapping
  write_entry(mapp_param_idx, entries_to_be_mapped);

  // update no. of mapped entries
  set_entry(mapp_param_idx, static_cast<uint8_t>(0x00),
                    static_cast<uint8_t>(entries_to_be_mapped.size()),
                    kaco::WriteAccessMethod::sdo);

  // set transmit type
  set_entry(comm_param_idx, static_cast<uint8_t>(0x02), transmit_type,
                    kaco::WriteAccessMethod::sdo);

  // set inhibit time
  set_entry(comm_param_idx, static_cast<uint8_t>(0x03), inhibit_time,
                    kaco::WriteAccessMethod::sdo);
                    
  // enable tpdo
  cob_id ^= static_cast<uint32_t>((0 ^ cob_id) & (1UL << 31));
  set_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                    static_cast<uint32_t>(cob_id),
                    kaco::WriteAccessMethod::sdo);
}

void Device::map_tpdo_in_device(TPDO_NO tpdo_no,
                        std::vector<uint32_t> entries_to_be_mapped,
                        uint8_t transmit_type) {
  auto tmp_idxs = get_tpdo_indexes(tpdo_no);

  uint16_t comm_param_idx = tmp_idxs.first;
  uint16_t mapp_param_idx = tmp_idxs.second;

  // disable tpdo
  uint32_t cob_id = 0;
  cob_id = get_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                             kaco::ReadAccessMethod::sdo);
  cob_id ^= static_cast<uint32_t>((-1 ^ cob_id) & (1UL << 31));
  set_entry(comm_param_idx, 0x01, static_cast<uint32_t>(cob_id),
                    kaco::WriteAccessMethod::sdo);

  // delete no. of mapped entries
  set_entry(mapp_param_idx, 0x00, static_cast<uint8_t>(0x00),
                    kaco::WriteAccessMethod::sdo);
                    
  // add new mapping
  write_entry(mapp_param_idx, entries_to_be_mapped);

  // update no. of mapped entries
  set_entry(mapp_param_idx, static_cast<uint8_t>(0x00),
                    static_cast<uint8_t>(entries_to_be_mapped.size()),
                    kaco::WriteAccessMethod::sdo);

  // set transmit type
  set_entry(comm_param_idx, static_cast<uint8_t>(0x02), transmit_type,
                    kaco::WriteAccessMethod::sdo);

  // enable tpdo
  cob_id ^= static_cast<uint32_t>((0 ^ cob_id) & (1UL << 31));
  set_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                    static_cast<uint32_t>(cob_id),
                    kaco::WriteAccessMethod::sdo);
}

void Device::map_rpdo_in_device(kaco::RPDO_NO rpdo_no,
                                std::vector<uint32_t> entries_to_be_mapped,
                                uint8_t transmit_type)
{
  auto tmp_idxs = get_rpdo_indexes(rpdo_no);

  uint16_t comm_param_idx = tmp_idxs.first;
  uint16_t mapp_param_idx = tmp_idxs.second;

  // disable rpdo1
  uint32_t cob_id = 0;
  cob_id = get_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                             kaco::ReadAccessMethod::sdo);
  cob_id ^= static_cast<uint32_t>((-1 ^ cob_id) & (1UL << 31));
  set_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                    static_cast<uint32_t>(cob_id),
                    kaco::WriteAccessMethod::sdo);

  // delete no. of mapped entries
  set_entry(mapp_param_idx, static_cast<uint8_t>(0x00),
                    static_cast<uint8_t>(0x00),
                    kaco::WriteAccessMethod::sdo);

  // add new mapping
  write_entry(mapp_param_idx, entries_to_be_mapped);

  // update no. of mapped entries (enable PDO)
  set_entry(mapp_param_idx, static_cast<uint8_t>(0x00),
                    static_cast<uint8_t>(entries_to_be_mapped.size()),
                    kaco::WriteAccessMethod::sdo);

  // set transmit type
  set_entry(comm_param_idx, static_cast<uint8_t>(0x02), transmit_type,
                    kaco::WriteAccessMethod::sdo);

  // enable rpdo1
  cob_id ^= static_cast<uint32_t>((0 ^ cob_id) & (1UL << 31));
  set_entry(comm_param_idx, static_cast<uint8_t>(0x01),
                    static_cast<uint32_t>(cob_id),
                    kaco::WriteAccessMethod::sdo);
}

} // end namespace kaco
