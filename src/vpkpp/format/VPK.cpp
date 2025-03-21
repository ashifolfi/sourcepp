#include <vpkpp/format/VPK.h>

#include <cstdio>
#include <filesystem>

#define CRYPTOPP_ENABLE_NAMESPACE_WEAK 1
#include <cryptopp/md5.h>

#include <FileStream.h>

#include <kvpp/kvpp.h>
#include <sourcepp/crypto/CRC32.h>
#include <sourcepp/crypto/MD5.h>
#include <sourcepp/crypto/RSA.h>
#include <sourcepp/crypto/String.h>
#include <sourcepp/FS.h>
#include <sourcepp/String.h>
#include <vpkpp/format/FPX.h>

using namespace kvpp;
using namespace sourcepp;
using namespace vpkpp;

/// Runtime-only flag that indicates a file is going to be written to an existing archive file
constexpr uint32_t VPK_FLAG_REUSING_CHUNK = 0x1;

namespace {

std::string removeVPKAndOrDirSuffix(const std::string& path, bool isFPX) {
	std::string filename = path;
	if (filename.length() >= 4 && filename.substr(filename.length() - 4) == (isFPX ? FPX_EXTENSION : VPK_EXTENSION)) {
		filename = filename.substr(0, filename.length() - 4);
	}

	// This indicates it's a dir VPK, but some people ignore this convention...
	// It should fail later if it's not a proper dir VPK
	if (filename.length() >= 4 && filename.substr(filename.length() - 4) == (isFPX ? FPX_DIR_SUFFIX : VPK_DIR_SUFFIX)) {
		filename = filename.substr(0, filename.length() - 4);
	}

	return filename;
}

bool isFPX(const VPK* vpk) {
	return vpk->getType() == PackFileType::FPX;
}

} // namespace

VPK::VPK(const std::string& fullFilePath_)
		: PackFile(fullFilePath_) {
	this->type = PackFileType::VPK;
}

std::unique_ptr<PackFile> VPK::create(const std::string& path, uint32_t version) {
	if (version != 1 && version != 2) {
		return nullptr;
	}

	{
		FileStream stream{path, FileStream::OPT_TRUNCATE | FileStream::OPT_CREATE_IF_NONEXISTENT};

		Header1 header1{};
		header1.signature = VPK_SIGNATURE;
		header1.version = version;
		header1.treeSize = 1;
		stream.write(header1);

		if (version != 1) {
			Header2 header2{};
			header2.fileDataSectionSize = 0;
			header2.archiveMD5SectionSize = 0;
			header2.otherMD5SectionSize = 0;
			header2.signatureSectionSize = 0;
			stream.write(header2);
		}

		stream.write('\0');
	}
	return VPK::open(path);
}

std::unique_ptr<PackFile> VPK::open(const std::string& path, const EntryCallback& callback) {
	auto vpk = VPK::openInternal(path, callback);
	if (!vpk && path.length() > 8) {
		// If it just tried to load a numbered archive, let's try to load the directory VPK
		if (auto dirPath = path.substr(0, path.length() - 8) + VPK_DIR_SUFFIX.data() + std::filesystem::path{path}.extension().string(); std::filesystem::exists(dirPath)) {
			vpk = VPK::openInternal(dirPath, callback);
		}
	}
	return vpk;
}

std::unique_ptr<PackFile> VPK::openInternal(const std::string& path, const EntryCallback& callback) {
	if (!std::filesystem::exists(path)) {
		// File does not exist
		return nullptr;
	}

	auto* vpk = new VPK{path};
	auto packFile = std::unique_ptr<PackFile>(vpk);

	FileStream reader{vpk->fullFilePath};
	reader.seek_in(0);
	reader.read(vpk->header1);
	if (vpk->header1.signature != VPK_SIGNATURE) {
		// File is not a VPK
		return nullptr;
	}
	if (vpk->header1.version == 2) {
		reader.read(vpk->header2);
	} else if (vpk->header1.version != 1) {
		// Apex Legends, Titanfall, etc. are not supported
		return nullptr;
	}

	// Extensions
	while (true) {
		std::string extension;
		reader.read(extension);
		if (extension.empty())
			break;

		// Directories
		while (true) {
			std::string directory;
			reader.read(directory);
			if (directory.empty())
				break;

			std::string fullDir;
			if (directory == " ") {
				fullDir = "";
			} else {
				fullDir = directory;
			}

			// Files
			while (true) {
				std::string entryName;
				reader.read(entryName);
				if (entryName.empty())
					break;

				Entry entry = createNewEntry();

				std::string entryPath;
				if (extension == " ") {
					entryPath = fullDir.empty() ? "" : fullDir + '/';
					entryPath += entryName;
				} else {
					entryPath = fullDir.empty() ? "" : fullDir + '/';
					entryPath += entryName + '.';
					entryPath += extension;
				}
				entryPath = vpk->cleanEntryPath(entryPath);

				reader.read(entry.crc32);
				auto preloadedDataSize = reader.read<uint16_t>();
				entry.archiveIndex = reader.read<uint16_t>();
				entry.offset = reader.read<uint32_t>();
				entry.length = reader.read<uint32_t>();

				if (reader.read<uint16_t>() != VPK_ENTRY_TERM) {
					// Invalid terminator!
					return nullptr;
				}

				if (preloadedDataSize > 0) {
					entry.extraData = reader.read_bytes(preloadedDataSize);
					entry.length += preloadedDataSize;
				}

				if (entry.archiveIndex != VPK_DIR_INDEX && entry.archiveIndex > vpk->numArchives) {
					vpk->numArchives = entry.archiveIndex;
				}

				vpk->entries.emplace(entryPath, entry);

				if (callback) {
					callback(entryPath, entry);
				}
			}
		}
	}

	// If there are no archives, -1 will be incremented to 0
	vpk->numArchives++;

	// Read VPK2-specific data
	if (vpk->header1.version != 2)
		return packFile;

	// Skip over file data, if any
	reader.seek_in(vpk->header2.fileDataSectionSize, std::ios::cur);

	if (vpk->header2.archiveMD5SectionSize % sizeof(MD5Entry) != 0)
		return nullptr;

	vpk->md5Entries.clear();
	unsigned int entryNum = vpk->header2.archiveMD5SectionSize / sizeof(MD5Entry);
	for (unsigned int i = 0; i < entryNum; i++)
		vpk->md5Entries.push_back(reader.read<MD5Entry>());

	if (vpk->header2.otherMD5SectionSize != 48)
		// This should always be 48
		return packFile;

	vpk->footer2.treeChecksum = reader.read_bytes<16>();
	vpk->footer2.md5EntriesChecksum = reader.read_bytes<16>();
	vpk->footer2.wholeFileChecksum = reader.read_bytes<16>();

	if (!vpk->header2.signatureSectionSize) {
		return packFile;
	}

	auto publicKeySize = reader.read<int32_t>();
	if (vpk->header2.signatureSectionSize == 20 && publicKeySize == VPK_SIGNATURE) {
		// CS2 beta VPK, ignore it
		return packFile;
	}

	vpk->footer2.publicKey = reader.read_bytes(publicKeySize);
	vpk->footer2.signature = reader.read_bytes(reader.read<int32_t>());

	return packFile;
}

std::vector<std::string> VPK::verifyEntryChecksums() const {
	return this->verifyEntryChecksumsUsingCRC32();
}

bool VPK::hasPackFileChecksum() const {
	return this->header1.version == 2;
}

bool VPK::verifyPackFileChecksum() const {
	// File checksums are only in v2
	if (this->header1.version != 2) {
		return true;
	}

	FileStream stream{this->getFilepath().data()};

	stream.seek_in(this->getHeaderLength());
	if (this->footer2.treeChecksum != crypto::computeMD5(stream.read_bytes(this->header1.treeSize))) {
		return false;
	}

	stream.seek_in(this->getHeaderLength() + this->header1.treeSize + this->header2.fileDataSectionSize);
	if (this->footer2.md5EntriesChecksum != crypto::computeMD5(stream.read_bytes(this->header2.archiveMD5SectionSize))) {
		return false;
	}

	stream.seek_in(0);
	if (this->footer2.wholeFileChecksum != crypto::computeMD5(stream.read_bytes(this->getHeaderLength() + this->header1.treeSize + this->header2.fileDataSectionSize + this->header2.archiveMD5SectionSize + this->header2.otherMD5SectionSize - sizeof(this->footer2.wholeFileChecksum)))) {
		return false;
	}

	return true;
}

bool VPK::hasPackFileSignature() const {
	if (this->header1.version != 2) {
		return false;
	}
	if (this->footer2.publicKey.empty() || this->footer2.signature.empty()) {
		return false;
	}
	return true;
}

bool VPK::verifyPackFileSignature() const {
	// Signatures are only in v2
	if (this->header1.version != 2) {
		return true;
	}

	if (this->footer2.publicKey.empty() || this->footer2.signature.empty()) {
		return true;
	}
	auto dirFileBuffer = fs::readFileBuffer(this->getFilepath().data());
	const auto signatureSectionSize = this->footer2.publicKey.size() + this->footer2.signature.size() + sizeof(uint32_t) * 2;
	if (dirFileBuffer.size() <= signatureSectionSize) {
		return false;
	}
	for (int i = 0; i < signatureSectionSize; i++) {
		dirFileBuffer.pop_back();
	}
	return crypto::verifySHA256PublicKey(dirFileBuffer, this->footer2.publicKey, this->footer2.signature);
}

std::optional<std::vector<std::byte>> VPK::readEntry(const std::string& path_) const {
	auto path = this->cleanEntryPath(path_);
	auto entry = this->findEntry(path);
	if (!entry) {
		return std::nullopt;
	}
	if (entry->unbaked) {
		return readUnbakedEntry(*entry);
	}

	std::vector output(entry->length, static_cast<std::byte>(0));

	if (!entry->extraData.empty()) {
		std::copy(entry->extraData.begin(), entry->extraData.end(), output.begin());
	}
	if (entry->length == entry->extraData.size()) {
		return output;
	}

	if (entry->archiveIndex != VPK_DIR_INDEX) {
		// Stored in a numbered archive
		FileStream stream{this->getTruncatedFilepath() + '_' + string::padNumber(entry->archiveIndex, 3) + (::isFPX(this) ? FPX_EXTENSION : VPK_EXTENSION).data()};
		if (!stream) {
			return std::nullopt;
		}
		stream.seek_in_u(entry->offset);
		auto bytes = stream.read_bytes(entry->length - entry->extraData.size());
		std::copy(bytes.begin(), bytes.end(), output.begin() + static_cast<long long>(entry->extraData.size()));
	} else {
		// Stored in this directory VPK
		FileStream stream{this->fullFilePath};
		if (!stream) {
			return std::nullopt;
		}
		stream.seek_in_u(this->getHeaderLength() + this->header1.treeSize + entry->offset);
		auto bytes = stream.read_bytes(entry->length - entry->extraData.size());
		std::copy(bytes.begin(), bytes.end(), output.begin() + static_cast<long long>(entry->extraData.size()));
	}

	return output;
}

void VPK::addEntryInternal(Entry& entry, const std::string& path, std::vector<std::byte>& buffer, EntryOptions options) {
	entry.crc32 = crypto::computeCRC32(buffer);
	entry.length = buffer.size();

	// Offset will be reset when it's baked, assuming we're not replacing an existing chunk (when flags = 1)
	entry.flags = 0;
	entry.offset = 0;
	entry.archiveIndex = options.vpk_saveToDirectory ? VPK_DIR_INDEX : this->numArchives;
	if (!options.vpk_saveToDirectory && !this->freedChunks.empty()) {
		int64_t bestChunkIndex = -1;
		std::size_t currentChunkGap = SIZE_MAX;
		for (int64_t i = 0; i < this->freedChunks.size(); i++) {
			if (
					(bestChunkIndex < 0 && this->freedChunks[i].length >= entry.length) ||
					(bestChunkIndex >= 0 && this->freedChunks[i].length >= entry.length && (this->freedChunks[i].length - entry.length) < currentChunkGap)
					) {
				bestChunkIndex = i;
				currentChunkGap = this->freedChunks[i].length - entry.length;
			}
		}
		if (bestChunkIndex >= 0) {
			entry.flags |= VPK_FLAG_REUSING_CHUNK;
			entry.offset = this->freedChunks[bestChunkIndex].offset;
			entry.archiveIndex = this->freedChunks[bestChunkIndex].archiveIndex;
			this->freedChunks.erase(this->freedChunks.begin() + bestChunkIndex);
			if (currentChunkGap < SIZE_MAX && currentChunkGap > 0) {
				// Add the remaining free space as a free chunk
				this->freedChunks.push_back({entry.offset + entry.length, currentChunkGap, entry.archiveIndex});
			}
		}
	}

	if (options.vpk_preloadBytes > 0) {
		auto clampedPreloadBytes = std::clamp(options.vpk_preloadBytes, 0u, buffer.size() > VPK_MAX_PRELOAD_BYTES ? VPK_MAX_PRELOAD_BYTES : static_cast<uint32_t>(buffer.size()));
		entry.extraData.resize(clampedPreloadBytes);
		std::memcpy(entry.extraData.data(), buffer.data(), clampedPreloadBytes);
		buffer.erase(buffer.begin(), buffer.begin() + clampedPreloadBytes);
	}

	// Now that archive index is calculated for this entry, check if it needs to be incremented
	if (!options.vpk_saveToDirectory && !(entry.flags & VPK_FLAG_REUSING_CHUNK)) {
		entry.offset = this->currentlyFilledChunkSize;
		this->currentlyFilledChunkSize += static_cast<int>(buffer.size());
		if (this->currentlyFilledChunkSize > this->chunkSize) {
			this->currentlyFilledChunkSize = 0;
			this->numArchives++;
		}
	}
}

bool VPK::removeEntry(const std::string& filename_) {
	auto filename = this->cleanEntryPath(filename_);
	if (auto entry = this->findEntry(filename); entry && (!entry->unbaked || entry->flags & VPK_FLAG_REUSING_CHUNK)) {
		this->freedChunks.push_back({entry->offset, entry->length, entry->archiveIndex});
	}
	return PackFile::removeEntry(filename);
}

std::size_t VPK::removeDirectory(const std::string& dirName_) {
	auto dirName = this->cleanEntryPath(dirName_);
	if (!dirName.empty()) {
		dirName += '/';
	}
	this->runForAllEntries([this, &dirName](const std::string& path, const Entry& entry) {
		if (path.starts_with(dirName) && (!entry.unbaked || entry.flags & VPK_FLAG_REUSING_CHUNK)) {
			this->freedChunks.push_back({entry.offset, entry.length, entry.archiveIndex});
		}
	});
	return PackFile::removeDirectory(dirName_);
}

bool VPK::bake(const std::string& outputDir_, BakeOptions options, const EntryCallback& callback) {
	// Get the proper file output folder
	std::string outputDir = this->getBakeOutputDir(outputDir_);
	std::string outputPath = outputDir + '/' + this->getFilename();

	// Reconstruct data so we're not looping over it a ton of times
	std::unordered_map<std::string, std::unordered_map<std::string, std::vector<std::pair<std::string, Entry*>>>> temp;
	this->runForAllEntriesInternal([&temp](const std::string& path, Entry& entry) {
		const auto fsPath = std::filesystem::path{path};
		auto extension = fsPath.extension().string();
		if (extension.starts_with('.')) {
			extension = extension.substr(1);
		}
		auto parentDir = fsPath.parent_path().string();

		if (extension.empty()) {
			extension = " ";
		}
		if (!temp.contains(extension)) {
			temp[extension] = {};
		}
		if (!temp.at(extension).contains(parentDir)) {
			temp.at(extension)[parentDir] = {};
		}
		temp.at(extension).at(parentDir).emplace_back(path, &entry);
	});

	// Temporarily store baked file data that's stored in the directory VPK since it's getting overwritten
	std::vector<std::byte> dirVPKEntryData;
	std::size_t newDirEntryOffset = 0;
	this->runForAllEntriesInternal([this, &dirVPKEntryData, &newDirEntryOffset](const std::string& path, Entry& entry) {
		if (entry.archiveIndex != VPK_DIR_INDEX || entry.length == entry.extraData.size()) {
			return;
		}

		auto binData = this->readEntry(path);
		if (!binData) {
			return;
		}
		dirVPKEntryData.reserve(dirVPKEntryData.size() + entry.length - entry.extraData.size());
		dirVPKEntryData.insert(dirVPKEntryData.end(), binData->begin() + static_cast<std::vector<std::byte>::difference_type>(entry.extraData.size()), binData->end());

		entry.offset = newDirEntryOffset;
		newDirEntryOffset += entry.length - entry.extraData.size();
	}, false);

	// Helper
	const auto getArchiveFilename = [this](const std::string& filename_, uint32_t archiveIndex) {
		std::string out{filename_ + '_' + string::padNumber(archiveIndex, 3) + (::isFPX(this) ? FPX_EXTENSION : VPK_EXTENSION).data()};
		string::normalizeSlashes(out);
		return out;
	};

	// Copy external binary blobs to the new dir
	if (!outputDir_.empty()) {
		for (uint32_t archiveIndex = 0; archiveIndex < this->numArchives; archiveIndex++) {
			std::string from = getArchiveFilename(this->getTruncatedFilepath(), archiveIndex);
			if (!std::filesystem::exists(from)) {
				continue;
			}
			std::string dest = getArchiveFilename(outputDir + '/' + this->getTruncatedFilestem(), archiveIndex);
			if (from == dest) {
				continue;
			}
			std::filesystem::copy_file(from, dest, std::filesystem::copy_options::overwrite_existing);
		}
	}

	FileStream outDir{outputPath, FileStream::OPT_READ | FileStream::OPT_TRUNCATE | FileStream::OPT_CREATE_IF_NONEXISTENT};
	outDir.seek_in(0);
	outDir.seek_out(0);

	// Dummy header
	outDir.write(this->header1);
	if (this->header1.version == 2) {
		outDir.write(this->header2);
	}

	// File tree data
	for (auto& [ext, dirs] : temp) {
		outDir.write(ext);

		for (auto& [dir, tempEntries] : dirs) {
			outDir.write(!dir.empty() ? dir : " ");

			for (auto& [path, entry] : tempEntries) {
				// Calculate entry offset if it's unbaked and upload the data
				if (entry->unbaked) {
					auto entryData = readUnbakedEntry(*entry);
					if (!entryData) {
						continue;
					}
					if (entry->length == entry->extraData.size()) {
						// Override the archive index, no need for an archive VPK
						entry->archiveIndex = VPK_DIR_INDEX;
						entry->offset = dirVPKEntryData.size();
					} else if (entry->archiveIndex != VPK_DIR_INDEX && (entry->flags & VPK_FLAG_REUSING_CHUNK)) {
						// The entry is replacing pre-existing data in a VPK archive
						auto archiveFilename = getArchiveFilename(::removeVPKAndOrDirSuffix(outputPath, ::isFPX(this)), entry->archiveIndex);
						FileStream stream{archiveFilename, FileStream::OPT_READ | FileStream::OPT_WRITE | FileStream::OPT_CREATE_IF_NONEXISTENT};
						stream.seek_out_u(entry->offset);
						stream.write(*entryData);
					} else if (entry->archiveIndex != VPK_DIR_INDEX) {
						// The entry is being appended to a newly created VPK archive
						auto archiveFilename = getArchiveFilename(::removeVPKAndOrDirSuffix(outputPath, ::isFPX(this)), entry->archiveIndex);
						entry->offset = std::filesystem::exists(archiveFilename) ? std::filesystem::file_size(archiveFilename) : 0;
						FileStream stream{archiveFilename, FileStream::OPT_APPEND | FileStream::OPT_CREATE_IF_NONEXISTENT};
						stream.write(*entryData);
					} else {
						// The entry will be added to the directory VPK
						entry->offset = dirVPKEntryData.size();
						dirVPKEntryData.insert(dirVPKEntryData.end(), entryData->data(), entryData->data() + entryData->size());
					}

					// Clear flags
					entry->flags = 0;
				}

				outDir.write(std::filesystem::path{path}.stem().string());
				outDir.write(entry->crc32);
				outDir.write<uint16_t>(entry->extraData.size());
				outDir.write<uint16_t>(entry->archiveIndex);
				outDir.write<uint32_t>(entry->offset);
				outDir.write<uint32_t>(entry->length - entry->extraData.size());
				outDir.write(VPK_ENTRY_TERM);

				if (!entry->extraData.empty()) {
					outDir.write(entry->extraData);
				}

				if (callback) {
					callback(path, *entry);
				}
			}
			outDir.write('\0');
		}
		outDir.write('\0');
	}
	outDir.write('\0');

	// Put files copied from the dir archive back
	if (!dirVPKEntryData.empty()) {
		outDir.write(dirVPKEntryData);
	}

	// Merge unbaked into baked entries
	this->mergeUnbakedEntries();

	// Calculate Header1
	this->header1.treeSize = outDir.tell_out() - dirVPKEntryData.size() - this->getHeaderLength();

	// VPK v2 stuff
	if (this->header1.version == 2) {
		// Calculate hashes for all entries
		this->md5Entries.clear();
		if (options.vpk_generateMD5Entries) {
			this->runForAllEntries([this](const std::string& path, const Entry& entry) {
				// Believe it or not this should be safe to call by now
				auto binData = this->readEntry(path);
				if (!binData) {
					return;
				}
				MD5Entry md5Entry{
						.archiveIndex = entry.archiveIndex,
						.offset = static_cast<uint32_t>(entry.offset),
						.length = static_cast<uint32_t>(entry.length - entry.extraData.size()),
						.checksum = crypto::computeMD5(*binData),
				};
				this->md5Entries.push_back(md5Entry);
			}, false);
		}

		// Calculate Header2
		this->header2.fileDataSectionSize = dirVPKEntryData.size();
		this->header2.archiveMD5SectionSize = this->md5Entries.size() * sizeof(MD5Entry);
		this->header2.otherMD5SectionSize = 48;
		this->header2.signatureSectionSize = 0;

		// Calculate Footer2
		CryptoPP::Weak::MD5 wholeFileChecksumMD5;
		{
			// Only the tree is updated in the file right now
			wholeFileChecksumMD5.Update(reinterpret_cast<const CryptoPP::byte*>(&this->header1), sizeof(Header1));
			wholeFileChecksumMD5.Update(reinterpret_cast<const CryptoPP::byte*>(&this->header2), sizeof(Header2));
		}
		{
			outDir.seek_in(sizeof(Header1) + sizeof(Header2));
			std::vector<std::byte> treeData = outDir.read_bytes(this->header1.treeSize);
			wholeFileChecksumMD5.Update(reinterpret_cast<const CryptoPP::byte*>(treeData.data()), treeData.size());
			this->footer2.treeChecksum = crypto::computeMD5(treeData);
		}
		if (!dirVPKEntryData.empty()) {
			wholeFileChecksumMD5.Update(reinterpret_cast<const CryptoPP::byte*>(dirVPKEntryData.data()), dirVPKEntryData.size());
		}
		{
			wholeFileChecksumMD5.Update(reinterpret_cast<const CryptoPP::byte*>(this->md5Entries.data()), this->md5Entries.size() * sizeof(MD5Entry));
			CryptoPP::Weak::MD5 md5EntriesChecksumMD5;
			md5EntriesChecksumMD5.Update(reinterpret_cast<const CryptoPP::byte*>(this->md5Entries.data()), this->md5Entries.size() * sizeof(MD5Entry));
			md5EntriesChecksumMD5.Final(reinterpret_cast<CryptoPP::byte*>(this->footer2.md5EntriesChecksum.data()));
		}
		wholeFileChecksumMD5.Update(reinterpret_cast<const CryptoPP::byte*>(this->footer2.treeChecksum.data()), this->footer2.treeChecksum.size());
		wholeFileChecksumMD5.Update(reinterpret_cast<const CryptoPP::byte*>(this->footer2.md5EntriesChecksum.data()), this->footer2.md5EntriesChecksum.size());
		wholeFileChecksumMD5.Final(reinterpret_cast<CryptoPP::byte*>(this->footer2.wholeFileChecksum.data()));

		// We can't recalculate the signature without the private key
		this->footer2.publicKey.clear();
		this->footer2.signature.clear();
	}

	// Write new headers
	outDir.seek_out(0);
	outDir.write(this->header1);

	// v2 adds the MD5 hashes and file signature
	if (this->header1.version != 2) {
		PackFile::setFullFilePath(outputDir);
		return true;
	}

	outDir.write(this->header2);

	// Add MD5 hashes
	outDir.seek_out_u(sizeof(Header1) + sizeof(Header2) + this->header1.treeSize + dirVPKEntryData.size());
	outDir.write(this->md5Entries);
	outDir.write(this->footer2.treeChecksum);
	outDir.write(this->footer2.md5EntriesChecksum);
	outDir.write(this->footer2.wholeFileChecksum);

	// The signature section is not present
	PackFile::setFullFilePath(outputDir);
	return true;
}

std::string VPK::getTruncatedFilestem() const {
	std::string filestem = this->getFilestem();
	// This indicates it's a dir VPK, but some people ignore this convention...
	if (filestem.length() >= 4 && filestem.substr(filestem.length() - 4) == (::isFPX(this) ? FPX_DIR_SUFFIX : VPK_DIR_SUFFIX)) {
		filestem = filestem.substr(0, filestem.length() - 4);
	}
	return filestem;
}

Attribute VPK::getSupportedEntryAttributes() const {
	using enum Attribute;
	return LENGTH | VPK_PRELOADED_DATA | ARCHIVE_INDEX | CRC32;
}

VPK::operator std::string() const {
	return PackFile::operator std::string() +
	       " | Version v" + std::to_string(this->header1.version);
}

bool VPK::generateKeyPairFiles(const std::string& name) {
	auto keys = crypto::computeSHA256KeyPair(1024);
	{
		auto privateKeyPath = name + ".privatekey.vdf";
		FileStream stream{privateKeyPath, FileStream::OPT_TRUNCATE | FileStream::OPT_CREATE_IF_NONEXISTENT};

		std::string output;
		// Template size, remove %s and %s, add key sizes, add null terminator size
		output.resize(VPK_KEYPAIR_PRIVATE_KEY_TEMPLATE.size() - 4 + keys.first.size() + keys.second.size() + 1);
		if (std::sprintf(output.data(), VPK_KEYPAIR_PRIVATE_KEY_TEMPLATE.data(), keys.first.data(), keys.second.data()) < 0) {
			return false;
		} else {
			output.pop_back();
			stream.write(output, false);
		}
	}
	{
		auto publicKeyPath = name + ".publickey.vdf";
		FileStream stream{publicKeyPath, FileStream::OPT_TRUNCATE | FileStream::OPT_CREATE_IF_NONEXISTENT};

		std::string output;
		// Template size, remove %s, add key size, add null terminator size
		output.resize(VPK_KEYPAIR_PUBLIC_KEY_TEMPLATE.size() - 2 + keys.second.size() + 1);
		if (std::sprintf(output.data(), VPK_KEYPAIR_PUBLIC_KEY_TEMPLATE.data(), keys.second.data()) < 0) {
			return false;
		} else {
			output.pop_back();
			stream.write(output, false);
		}
	}
	return true;
}

bool VPK::sign(const std::string& filename_) {
	if (this->header1.version != 2 || !std::filesystem::exists(filename_) || std::filesystem::is_directory(filename_)) {
		return false;
	}

	KV1 fileKV{fs::readFileText(filename_)};

	auto privateKeyHex = fileKV["private_key"]["rsa_private_key"].getValue();
	if (privateKeyHex.empty()) {
		return false;
	}
	auto publicKeyHex = fileKV["private_key"]["public_key"]["rsa_public_key"].getValue();
	if (publicKeyHex.empty()) {
		return false;
	}

	return this->sign(crypto::decodeHexString(privateKeyHex), crypto::decodeHexString(publicKeyHex));
}

bool VPK::sign(const std::vector<std::byte>& privateKey, const std::vector<std::byte>& publicKey) {
	if (this->header1.version != 2) {
		return false;
	}

	this->header2.signatureSectionSize = this->footer2.publicKey.size() + this->footer2.signature.size() + sizeof(uint32_t) * 2;
	{
		FileStream stream{this->getFilepath().data(), FileStream::OPT_READ | FileStream::OPT_WRITE};
		stream.seek_out(sizeof(Header1));
		stream.write(this->header2);
	}

	auto dirFileBuffer = fs::readFileBuffer(this->getFilepath().data());
	if (dirFileBuffer.size() <= this->header2.signatureSectionSize) {
		return false;
	}
	for (int i = 0; i < this->header2.signatureSectionSize; i++) {
		dirFileBuffer.pop_back();
	}
	this->footer2.publicKey = publicKey;
	this->footer2.signature = crypto::signDataWithSHA256PrivateKey(dirFileBuffer, privateKey);

	{
		FileStream stream{this->getFilepath().data(), FileStream::OPT_READ | FileStream::OPT_WRITE};
		stream.seek_out(this->getHeaderLength() + this->header1.treeSize + this->header2.fileDataSectionSize + this->header2.archiveMD5SectionSize + this->header2.otherMD5SectionSize);
		stream.write(static_cast<uint32_t>(this->footer2.publicKey.size()));
		stream.write(this->footer2.publicKey);
		stream.write(static_cast<uint32_t>(this->footer2.signature.size()));
		stream.write(this->footer2.signature);
	}
	return true;
}

uint32_t VPK::getVersion() const {
	return this->header1.version;
}

void VPK::setVersion(uint32_t version) {
	if (version != 1 && version != 2) {
		return;
	}
	if (::isFPX(this) || version == this->header1.version) {
		return;
	}
	this->header1.version = version;

	// Clearing these isn't necessary, but might as well
	this->header2 = Header2{};
	this->footer2 = Footer2{};
	this->md5Entries.clear();
}

uint32_t VPK::getChunkSize() const {
	return this->chunkSize;
}

void VPK::setChunkSize(uint32_t newChunkSize) {
	this->chunkSize = newChunkSize;
}

uint32_t VPK::getHeaderLength() const {
	if (this->header1.version != 2) {
		return sizeof(Header1);
	}
	return sizeof(Header1) + sizeof(Header2);
}
