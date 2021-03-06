#ifndef __H_KVDB__
#define __H_KVDB__


#include <iostream>
#include <stdio.h>
#include <fstream>
#include <stdint.h>
#include <unordered_map>
#include <array>
#include <vector>
#include <memory>
#include <string>
#include <list>
#include <set>
#include <mutex>
#include <shared_mutex>
#include <cassert>
#include <cstring> 


#define KVDB_KEY_SIZE 64
#define KVDB_RESERVED_TABLE_SIZE 5

typedef uint32_t uint32;
typedef unsigned long long ulong64;
typedef unsigned char byte;

typedef std::array<byte, KVDB_KEY_SIZE> TKeyData;
typedef std::vector<byte> TValueData;
typedef std::shared_ptr<TValueData> TValueDataPtr;

//============================================================================
// Key data hash
//============================================================================
namespace std {
	template <>
	struct hash<TKeyData> {
		std::size_t operator()(const TKeyData& keyData) const {
			std::size_t h = 0;
			for (auto elem : keyData) {
				h ^= std::hash<int>{}(elem)+0x9e3779b9 + (h << 6) + (h >> 2);
			}
			return h;
		}
	};
}

namespace kvdb {

	//============================================================================
	// Base IO
	//============================================================================
	template <typename T>
	void write(std::ostream* os, const T& obj) {
		if (os != nullptr) {
			os->write((char*)&obj, sizeof(obj));
		}
	}

	template <typename T>
	void read(std::istream* is, const T& obj) {
		if (is != nullptr) {
			is->read((char*)&obj, sizeof(obj));
		}
	}

	//============================================================================
	// File position
	//============================================================================
	template <typename T>
	class TPosWrapper {

	private:
		T objT;

	public:
		ulong64 pos;

		TPosWrapper() {};

		TPosWrapper(T t, ulong64 p) : objT(t), pos(p) {};

		const T& operator()() const { return objT; }

		T& operator()() { return objT; }

		T* operator->() { return &objT; }
	};

	//============================================================================
	// File header
	//============================================================================
	typedef struct TFileHeader {
		uint32 version = 1;
		uint32 keySize = 0;
		ulong64 timestamp = 0;
		uint32  endOfHeaderOffset = 0;
	} TFileHeader;

	inline std::ostream* operator << (std::ostream* os, const TFileHeader& obj) {
		write(os, obj);
		return os;
	}

	inline std::istream* operator >> (std::istream* is, TFileHeader& obj) {
		read(is, obj);
		return is;
	}

	//============================================================================
	// Table header
	//============================================================================
	typedef struct TTableHeader {
		ulong64 recordCount = 0;
		ulong64 nextTable = 0;
	} TTableHeader;

	typedef TPosWrapper<TTableHeader> TTableHeaderInfo;

	inline std::ostream* operator << (std::ostream* os, const TTableHeader& obj) {
		write(os, obj);
		return os;
	}

	inline std::istream* operator >> (std::istream* is, TTableHeader& obj) {
		read(is, obj);
		return is;
	}

	//============================================================================
	// Key Entry
	//============================================================================
	typedef struct TKeyEntry {
		ulong64 dataPos = 0;
		ulong64 dataLength = 0;
		ulong64 initialDataLength = 0;

		TKeyData freeKeyData;

		TKeyEntry() {
			for (int i = 0; i < KVDB_KEY_SIZE; i++) freeKeyData[i] = 0;
		}

	} TKeyEntry;

	typedef TPosWrapper<TKeyEntry> TKeyEntryInfo;

	struct TKeyInfoComparatorByInitialLength {
		bool operator() (const TKeyEntryInfo &lhs, const TKeyEntryInfo &rhs) const {
			return (lhs().initialDataLength > rhs().initialDataLength);
		}
	};

	inline std::ostream* operator << (std::ostream* os, const TKeyEntry& obj) {
		write(os, obj);
		return os;
	}

	inline std::istream* operator >> (std::istream* is, TKeyEntry& obj) {
		read(is, obj);
		return is;
	}

	//============================================================================
	// File db
	//============================================================================
	template <typename K, typename V>
	class KvFile {

	private:

		std::unordered_map<TKeyData, TKeyEntryInfo> dataMap;
		std::fstream* filePtr = nullptr;
		std::list<TKeyEntryInfo> reservedKeyList;
		std::set<TKeyEntryInfo, TKeyInfoComparatorByInitialLength> deletedKeyList;
		std::list<TTableHeaderInfo> tableList;
		mutable std::shared_mutex fileSharedMutex;

	private:

		std::shared_ptr<V> valueFromData(TValueDataPtr dataPtr) {
			if (dataPtr == nullptr) return nullptr;

			V* temp = new V();
			memcpy(temp, dataPtr->data(), dataPtr->size());

			return std::shared_ptr<V>(temp);
		}

		static TKeyData toKeyData(K key) {
			byte temp[KVDB_KEY_SIZE];
			for (int i = 0; i < KVDB_KEY_SIZE; i++) temp[i] = 0;

			memcpy(&temp, &key, sizeof(K));

			TKeyData arrayOfByte;
			for (int i = 0; i < KVDB_KEY_SIZE; i++) arrayOfByte[i] = temp[i];

			return arrayOfByte;
		}

		static TValueData toValueData(V value) {
			std::vector<byte> temp;

			if (temp.size() < sizeof(value)) temp.resize(sizeof(value));
			std::memcpy(temp.data(), &value, sizeof(value));

			return temp;
		}

		void rewritePair(TKeyEntryInfo& keyInfo, const TValueData& valueData) {
			// rewrite value data
			filePtr->seekg(keyInfo().dataPos);
			filePtr->write((char*)valueData.data(), valueData.size());

			// rewrite key data
			keyInfo().dataLength = valueData.size(); // new length
			filePtr->seekg(keyInfo.pos);
			filePtr << keyInfo();
		}

		void earsePair(TKeyEntryInfo& keyInfo) {
			// rewrite key data
			keyInfo().dataLength = 0; // new length
			filePtr->seekg(keyInfo.pos);
			filePtr << keyInfo();

			deletedKeyList.insert(keyInfo);
			dataMap.erase(keyInfo().freeKeyData);
		}

		void newPairFromReserved(const TKeyData& keyData, const TValueData& valueData) {
			// has reserved key slots
			TKeyEntryInfo& keyInfo = reservedKeyList.front();

			filePtr->seekg(0, std::ios::end); // to end-of-file
			ulong64 endFile = (ulong64)(filePtr->tellp());
			filePtr->write((char*)valueData.data(), valueData.size());

			// fill key data
			keyInfo().dataLength = valueData.size(); // length
			keyInfo().initialDataLength = valueData.size(); // length
			keyInfo().dataPos = endFile;
			keyInfo().freeKeyData = keyData;

			filePtr->seekg(keyInfo.pos);
			filePtr << keyInfo();

			// add new pair to table 
			dataMap.insert({ keyInfo().freeKeyData, keyInfo });
			reservedKeyList.pop_front();
		}

		ulong64 readTable() {
			int tablePos = (int)filePtr->tellp();

			TTableHeader tableHeader;
			filePtr >> tableHeader;

			for (int i = 0; i < tableHeader.recordCount; i++) {
				ulong64 pos = (ulong64)filePtr->tellp();

				TKeyEntry keyEntry;
				filePtr >> keyEntry;

				TKeyEntryInfo keyInfo(keyEntry, pos);

				if (keyInfo().dataLength > 0) {
					dataMap.insert({ keyEntry.freeKeyData, keyInfo });
				}
				else {
					if (keyInfo().initialDataLength == 0) {
						// reserved key slot
						reservedKeyList.push_back(keyInfo);
					}
					else {
						// marked as deleted pair
						deletedKeyList.insert(keyInfo);
					}
				}
			}

			tableList.push_back(TTableHeaderInfo(tableHeader, tablePos));
			return tableHeader.nextTable;
		}

		void createNewTable() {
			filePtr->seekg(0, std::ios::end); // to end-of-file
			int newTablePos = (int)filePtr->tellp();

			// write new table
			TTableHeader newTable;
			newTable.recordCount = KVDB_RESERVED_TABLE_SIZE;
			newTable.nextTable = 0;
			filePtr << newTable;

			// write reserved keys
			for (int i = 0; i < KVDB_RESERVED_TABLE_SIZE; i++) {
				int newReservedKeyPos = (int)filePtr->tellp();

				TKeyEntry newReservedKey;
				filePtr << newReservedKey;

				TKeyEntryInfo keyInfo(newReservedKey, newReservedKeyPos);
				reservedKeyList.push_back(keyInfo);
			}

			// read previous last table 
			TTableHeaderInfo& lastTable = tableList.back();
			filePtr->seekg(lastTable.pos);

			// add link to new table
			lastTable().nextTable = newTablePos;

			// rewrite previous last table
			filePtr << lastTable();

			// add new table to internal list
			tableList.push_back(TTableHeaderInfo(newTable, newTablePos));
		}

		bool hasReserved() {
			return reservedKeyList.size() > 0;
		}

		bool tryWriteToSuitableDeletedPair(const TKeyData& keyData, const TValueData& valueData) {
			for (auto itr = deletedKeyList.begin(); itr != deletedKeyList.end();) {
				TKeyEntryInfo keyInfo = *itr;
				if (keyInfo().initialDataLength >= valueData.size()) {
					TKeyEntryInfo keyInfo = *itr;
					rewritePair(keyInfo, valueData);
					dataMap.insert({ keyInfo().freeKeyData, keyInfo });
					itr = deletedKeyList.erase(itr);

					return true;
				}
				else {
					++itr;
				}
			}

			return false;
		}

		void addNew(const TKeyData& keyData, const TValueData& valueData) {
			if (valueData.size() == 0) {
				return;
			}

			if (!tryWriteToSuitableDeletedPair(keyData, valueData)) {
				if (hasReserved()) {
					newPairFromReserved(keyData, valueData);
				}
				else {
					createNewTable();
					newPairFromReserved(keyData, valueData);
				}
			}
		}

		void change(const TKeyData& keyData, const TValueData& valueData) {
			TKeyEntryInfo keyInfo = dataMap[keyData];
			if (valueData.size() > 0) {
				if (keyInfo().dataLength >= valueData.size()) {
					rewritePair(keyInfo, valueData);
				}
				else {
					//remove old and create new
					earsePair(keyInfo);
					addNew(keyData, valueData);
				}
			}
			else {
				// erase
				earsePair(keyInfo);
			}
		}

	public:

		KvFile() {
			assert(sizeof(K) <= KVDB_KEY_SIZE);
		}

		void close() {
			if (!filePtr->is_open()) return;

			filePtr->close();
			dataMap.clear();
			reservedKeyList.clear();
			deletedKeyList.clear();
			tableList.clear();
		}

		bool open(std::string& file) {
			filePtr = new std::fstream(file, std::ios::in | std::ios::out | std::ios::binary);

			if (!filePtr->is_open()) return false;

			TFileHeader fileHeader;
			filePtr >> fileHeader;

			ulong64 nextTablePos = readTable();
			while (nextTablePos > 0) {
				filePtr->seekg(nextTablePos);
				nextTablePos = readTable();
			}
            
			return true;
		}

		int size() {
			if (!filePtr->is_open()) {
				return 0;
			} else {
				return dataMap.size();
			}
		}

		TValueDataPtr get(const K& k) const {
			const TKeyData keyData = toKeyData(k);

			if (!filePtr->is_open()) return nullptr;
            std::shared_lock<std::shared_mutex> lock(fileSharedMutex);

			std::unordered_map<TKeyData, TKeyEntryInfo>::const_iterator itr = dataMap.find(keyData);
			if (itr == dataMap.end()) {
				return nullptr;
			}

			const TKeyEntry& e = ((const TKeyEntryInfo&)(itr->second))();
			filePtr->seekg(e.dataPos);

			TValueDataPtr dataPtr = TValueDataPtr(new TValueData);
			for (int i = 0; i < e.dataLength; i++) {
				byte tmp;
				read(filePtr, tmp);
				dataPtr->push_back(tmp);
			}

			return dataPtr;
		}

		std::shared_ptr<V> getVal(const K& k) {
			return valueFromData(get(k));
		}

		void erase(const K& k) {
			TKeyData keyData = toKeyData(k);

			if (!filePtr->is_open()) return;
            std::unique_lock<std::shared_mutex> lock(fileSharedMutex);

			std::unordered_map<TKeyData, TKeyEntryInfo>::const_iterator itr = dataMap.find(keyData);
			if (itr != dataMap.end()) {
				TKeyEntryInfo& keyInfo = dataMap[keyData];
				earsePair(keyInfo);
			}
		}


		void put(const K& k, const V& v) {
			TKeyData keyData = toKeyData(k);
			TValueData valueData;

			// TODO fixme
			if (typeid(v) == typeid(TValueData)) {
				valueData = (TValueData)v;
			} else {
				valueData = toValueData(v);
			}

			if (!filePtr->is_open()) return;
            std::unique_lock<std::shared_mutex> lock(fileSharedMutex);

			std::unordered_map<TKeyData, TKeyEntryInfo>::const_iterator itr = dataMap.find(keyData);
			if (itr == dataMap.end()) {
				// pair not found  
				addNew(keyData, valueData);
			} else {
				// pair found  
				change(keyData, valueData);
			}
		}

		static bool create(std::string& file, const std::unordered_map<K, V>& test) {
			std::ofstream outFile(file, std::ios::out | std::ios::binary);

			if (!outFile) return false;

			std::ofstream* outFilePtr = &outFile;

			static const int max_key_records = KVDB_RESERVED_TABLE_SIZE;
			const int keyRecords = (test.size() > max_key_records) ? test.size() : max_key_records;

			// save file header
			TFileHeader fileHeader;
			fileHeader.keySize = KVDB_KEY_SIZE;
			fileHeader.endOfHeaderOffset = sizeof(fileHeader);

			outFilePtr << fileHeader;

			TTableHeader tableHeader;
			tableHeader.recordCount = keyRecords;
			outFilePtr << tableHeader;

			int bodyDataOffset = (int)(outFile.tellp()) + sizeof(TKeyEntry) * keyRecords;

			std::vector<byte> dataBody;

			for (auto& e : test) {
				TKeyEntry entry;
				entry.freeKeyData = toKeyData(e.first);
				entry.dataPos = dataBody.size() + bodyDataOffset;

				// TODO fixme
				if (typeid(e.second) == typeid(TValueData)) {
					const TValueData& valueData = (const TValueData&)e.second;
					entry.dataLength = valueData.size();
					entry.initialDataLength = valueData.size();
					dataBody.insert(std::end(dataBody), std::begin(valueData), std::end(valueData));
				} else {
					TValueData valueData = toValueData(e.second);
					entry.dataLength = valueData.size();
					entry.initialDataLength = valueData.size();
					dataBody.insert(std::end(dataBody), std::begin(valueData), std::end(valueData));
				}

				outFilePtr << entry;
			}

			if (test.size() < max_key_records) {
				// add empty records
				const int emptyRecords = max_key_records - test.size();
				for (int i = 0; i < emptyRecords; i++) {
					TKeyEntry entry;
					outFilePtr << entry;
				}
			}

			outFilePtr->write((char*)dataBody.data(), dataBody.size());
			outFile.close();

			return true;
		}
	};

	//-----------------------------------------------------------------------------

}

#endif