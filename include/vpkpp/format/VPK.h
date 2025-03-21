#pragma once

#include <array>
#include <tuple>

#include "../PackFile.h"

namespace vpkpp {

constexpr uint32_t VPK_SIGNATURE = 0x55aa1234;
constexpr uint16_t VPK_DIR_INDEX = 0x7fff;
constexpr uint16_t VPK_ENTRY_TERM = 0xffff;
constexpr std::string_view VPK_DIR_SUFFIX = "_dir";
constexpr std::string_view VPK_EXTENSION = ".vpk";

constexpr std::string_view VPK_KEYPAIR_PUBLIC_KEY_TEMPLATE = "public_key\n{\n\ttype \"rsa\"\n\trsa_public_key \"%s\"\n}\n";
constexpr std::string_view VPK_KEYPAIR_PRIVATE_KEY_TEMPLATE = "private_key\n{\n\ttype \"rsa\"\n\trsa_private_key \"%s\"\n\tprivate_key_encrypted 0\n\tpublic_key\n\t{\n\t\ttype \"rsa\"\n\t\trsa_public_key \"%s\"\n\t}\n}\n";

/// Maximum preload data size in bytes
constexpr uint32_t VPK_MAX_PRELOAD_BYTES = 1024;

/// Chunk size in bytes (default is 200mb)
constexpr uint32_t VPK_DEFAULT_CHUNK_SIZE = 200 * 1024 * 1024;

class VPK : public PackFile {
protected:
#pragma pack(push, 1)
	struct Header1 {
		uint32_t signature;
		uint32_t version;
		uint32_t treeSize;
	};

	struct Header2 {
		uint32_t fileDataSectionSize;
		uint32_t archiveMD5SectionSize;
		uint32_t otherMD5SectionSize;
		uint32_t signatureSectionSize;
	};

	struct Footer2 {
		std::array<std::byte, 16> treeChecksum{};
		std::array<std::byte, 16> md5EntriesChecksum{};
		std::array<std::byte, 16> wholeFileChecksum{};
		std::vector<std::byte> publicKey{};
		std::vector<std::byte> signature{};
	};

	struct MD5Entry {
		/// The archive index of the file
		uint32_t archiveIndex;
		/// The offset in the archive
		uint32_t offset;
		/// The length in bytes
		uint32_t length;
		/// The CRC32 checksum of this entry
		std::array<std::byte, 16> checksum;
	};
#pragma pack(pop)

	struct FreedChunk {
		uint64_t offset;
		uint64_t length;
		uint32_t archiveIndex;
	};

public:
	/// Create a new directory VPK file - should end in "_dir.vpk"! This is not enforced but STRONGLY recommended
	static std::unique_ptr<PackFile> create(const std::string& path, uint32_t version = 2);

	/// Open a VPK file
	[[nodiscard]] static std::unique_ptr<PackFile> open(const std::string& path, const EntryCallback& callback = nullptr);

	[[nodiscard]] constexpr bool hasEntryChecksums() const override {
		return true;
	}

	[[nodiscard]] std::vector<std::string> verifyEntryChecksums() const override;

	[[nodiscard]] bool hasPackFileChecksum() const override;

	[[nodiscard]] bool verifyPackFileChecksum() const override;

	[[nodiscard]] bool hasPackFileSignature() const override;

	[[nodiscard]] bool verifyPackFileSignature() const override;

	[[nodiscard]] std::optional<std::vector<std::byte>> readEntry(const std::string& path_) const override;

	bool removeEntry(const std::string& filename_) override;

	std::size_t removeDirectory(const std::string& dirName_) override;

	bool bake(const std::string& outputDir_ /*= ""*/, BakeOptions options /*= {}*/, const EntryCallback& callback /*= nullptr*/) override;

	[[nodiscard]] std::string getTruncatedFilestem() const override;

	[[nodiscard]] Attribute getSupportedEntryAttributes() const override;

	[[nodiscard]] explicit operator std::string() const override;

	/// Generate keypair files, which can be used to sign a VPK
	/// Input is a truncated file path, e.g. "/x/y/z/key" or just "key" for the CWD
	/// It will append ".publickey.vdf" and ".privatekey.vdf" to the input and save those files
	static bool generateKeyPairFiles(const std::string& name);

	/// Sign the VPK with the given private key KeyValues file. (See below comment)
	bool sign(const std::string& filename_);

	/// Sign the VPK with the given keypair. Run this after VPK::bake if modifying the VPK file contents
	bool sign(const std::vector<std::byte>& privateKey, const std::vector<std::byte>& publicKey);

	/// Returns 1 for v1, 2 for v2
	[[nodiscard]] uint32_t getVersion() const;

	/// Change the version of the VPK. Valid values are 1 and 2
	void setVersion(uint32_t version);

	/// Get the VPK chunk size in bytes (size of generated archives when baking)
	[[nodiscard]] uint32_t getChunkSize() const;

	/// Set the VPK chunk size in bytes (size of generated archives when baking)
	void setChunkSize(uint32_t newChunkSize);

protected:
	explicit VPK(const std::string& fullFilePath_);

	[[nodiscard]] static std::unique_ptr<PackFile> openInternal(const std::string& path, const EntryCallback& callback = nullptr);

	void addEntryInternal(Entry& entry, const std::string& path, std::vector<std::byte>& buffer, EntryOptions options) override;

	[[nodiscard]] uint32_t getHeaderLength() const;

	uint32_t numArchives = -1;
	uint32_t currentlyFilledChunkSize = 0;
	uint32_t chunkSize = VPK_DEFAULT_CHUNK_SIZE;

	std::vector<FreedChunk> freedChunks;

	Header1 header1{}; // Present in all VPK versions
	Header2 header2{}; // Present in VPK v2
	Footer2 footer2{}; // Present in VPK v2

	std::vector<MD5Entry> md5Entries;

private:
	VPKPP_REGISTER_PACKFILE_OPEN(VPK_EXTENSION, &VPK::open);
};

} // namespace vpkpp
