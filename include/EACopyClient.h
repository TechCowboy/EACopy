// (c) Electronic Arts. All Rights Reserved.

#pragma once
#include "EACopyNetwork.h"

namespace eacopy
{

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

constexpr char ClientVersion[] = "0.9982" CFG_STR; // Version of client (visible when printing help info)

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

enum FileFlags { FileFlags_Data = 1, FileFlags_Attributes = 2, FileFlags_Timestamps = 4 };
enum UseServer { UseServer_Automatic, UseServer_Required, UseServer_Disabled };


///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ClientSettings
{
	using				StringList = List<WString>;

	WString				sourceDirectory;
	WString				destDirectory;
	StringList			filesOrWildcards;
	StringList			filesOrWildcardsFiles;
	StringList			filesExcludeFiles;
	StringList			excludeWildcards;
	StringList			excludeWildcardDirectories;
	StringList			optionalWildcards; // Will not causes error if source file fulfill optionalWildcards
	uint				threadCount					= 0;
	uint				retryWaitTimeMs				= 30 * 1000;
	uint				retryCount					= 1000000;
	int					dirCopyFlags				= FileFlags_Data | FileFlags_Attributes;
	bool				forceCopy					= false;
	bool				flattenDestination			= false;
	int					copySubdirDepth				= 0;
	bool				copyEmptySubdirectories		= false;
	bool				purgeDestination			= false;
	UseServer			useServer					= UseServer_Automatic;
	WString				serverAddress;
	uint				serverPort					= DefaultPort;
	uint				serverConnectTimeoutMs		= 500;
	u64					deltaCompressionThreshold	= ~u64(0);
	bool				compressionEnabled			= false;
	int					compressionLevel			= 0;
	bool				logProgress					= true;
	bool				logDebug					= false;
	UseBufferedIO		useBufferedIO				= UseBufferedIO_Auto;
	bool				replaceSymLinksAtDestination= true; // When writing to destination and a directory is a symlink we remove symlink and create real directory
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

struct ClientStats
{
	u64					copyCount					= 0;
	u64					copySize					= 0;
	u64					copyTimeMs					= 0;
	u64					skipCount					= 0;
	u64					skipSize					= 0;
	u64					skipTimeMs					= 0;
	u64					linkCount					= 0;
	u64					linkSize					= 0;
	u64					linkTimeMs					= 0;
	u64					createDirCount				= 0;
	u64					createDirTimeMs				= 0;
	u64					failCount					= 0;
	u64					retryCount					= 0;
	u64					connectTimeMs				= 0;
	u64					sendTimeMs					= 0;
	u64					sendSize					= 0;
	u64					recvTimeMs					= 0;
	u64					recvSize					= 0;
	u64					purgeTimeMs					= 0;
	u64					compressTimeMs				= 0;
	u64					compressionLevelSum			= 0;
	float				compressionAverageLevel		= 0;
	u64					decompressTimeMs			= 0;
	u64					deltaCompressionTimeMs		= 0;
	CopyStats			copyStats;

	bool				destServerUsed				= false;
	bool				sourceServerUsed			= false;
	bool				serverAttempt				= false;
	u64					findFileTimeMs				= 0;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Client
{
public:
						// Ctor
						Client(const ClientSettings& settings);

						// Process files from source to dest
	int					process(Log& log);
	int					process(Log& log, ClientStats& outStats);

						// Report server status using destination path
	int					reportServerStatus(Log& log);

private:

	// Types
	struct				CopyEntry { WString src; WString dst; FileInfo srcInfo; };
	using				HandleFileFunc = Function<bool()>;
	using				HandleFileOrWildcardFunc = Function<bool(char*)>;
	using				CopyEntries = List<CopyEntry>;
	class				Connection;
	struct				NameAndFileInfo { WString name; FileInfo info; DWORD attributes; };

	// Methods
	void				resetWorkState(Log& log);
	bool				processFile(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats);
	bool				processFiles(LogContext& logContext, Connection* sourceConnection, Connection* destConnection, NetworkCopyContext& copyContext, ClientStats& stats, bool isMainThread);
	bool				connectToServer(const wchar_t* networkPath, uint connectionIndex, class Connection*& outConnection, bool& failedToConnect, ClientStats& stats);
	int					workerThread(uint connectionIndex, ClientStats& stats);
	bool				findFilesInDirectory(const WString& sourcePath, const WString& destPath, const WString& wildcard, int depthLeft, const HandleFileFunc& handleFileFunc, ClientStats& stats);
	bool				handleFile(const WString& sourcePath, const WString& destPath, const wchar_t* fileName, const FileInfo& fileInfo, const HandleFileFunc& handleFileFunc);
	bool				handleDirectory(const WString& sourcePath, const WString& destPath, const wchar_t* directory, const wchar_t* wildcard, int depthLeft, const HandleFileFunc& handleFileFunc, ClientStats& stats);
	bool				handlePath(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& destPath, const wchar_t* fileName, const HandleFileFunc& handleFileFunc);
	bool				handleFilesOrWildcardsFromFile(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& fileName, const WString& destPath, const HandleFileOrWildcardFunc& func);
	bool				excludeFilesFromFile(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& fileName, const WString& destPath);
	bool				gatherFilesOrWildcardsFromFile(LogContext& logContext, ClientStats& stats, const WString& sourcePath, const WString& fileName, const WString& destPath, const HandleFileFunc& handleFileFunc);
	bool				purgeFilesInDirectory(const WString& destPath, DWORD destPathAttributes, int depthLeft);
	bool				ensureDirectory(const wchar_t* directory);
	const wchar_t*		getRelativeSourceFile(const WString& sourcePath) const;
	Connection*			createConnection(const wchar_t* networkPath, uint connectionIndex, ClientStats& stats, bool& failedToConnect, bool doProtocolCheck);
	bool				isIgnoredDirectory(const wchar_t* directory);
	bool				isValid(Connection* connection);


	// Settings
	const ClientSettings& m_settings;

	// Work state. Will initialize at beginning of each process call.
	Log*				m_log;
	bool				m_useSourceServerFailed;
	bool				m_useDestServerFailed;
	Event				m_workDone;
	bool				m_tryCopyFirst;
	NetworkCopyContext	m_copyContext;
	Connection*			m_sourceConnection;
	Connection*			m_destConnection;
	CriticalSection		m_copyEntriesCs;
	CopyEntries			m_copyEntries;
	FilesSet			m_handledFiles;
	FilesSet			m_createdDirs;
	FilesSet			m_purgeDirs;
	CriticalSection		m_networkInitCs;
	bool				m_networkWsaInitDone;
	bool				m_networkInitDone;
	WString				m_networkServerName;
	WString				m_networkServerNetDirectory;
	addrinfoW*			m_serverAddrInfo;

						Client(const Client&) = delete;
	void				operator=(const Client&) = delete;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class Client::Connection
{
public:
						Connection(const ClientSettings& settings, ClientStats& stats, Socket s);
						~Connection();
	bool				sendCommand(const Command& cmd);
	bool				sendTextCommand(const wchar_t* text);
	bool				sendWriteFileCommand(const wchar_t* src, const wchar_t* dst, const FileInfo& srcInfo, u64& outSize, u64& outWritten, bool& outLinked, CopyContext& copyContext);

	enum				ReadFileResult { ReadFileResult_Error, ReadFileResult_Success, ReadFileResult_ServerBusy };
	ReadFileResult		sendReadFileCommand(const wchar_t* src, const wchar_t* dst, const FileInfo& srcInfo, u64& outSize, u64& outRead, NetworkCopyContext& copyContext);


	bool				sendCreateDirectoryCommand(const wchar_t* directory, FilesSet& outCreatedDirs);
	bool				sendDeleteAllFiles(const wchar_t* dir);
	bool				sendFindFiles(const wchar_t* dirAndWildcard, Vector<NameAndFileInfo>& outFiles, CopyContext& copyContext);
	bool				sendGetFileAttributes(const wchar_t* file, FileInfo& outInfo, DWORD& outAttributes, DWORD& outError);

	bool				destroy();

	const ClientSettings& m_settings;
	ClientStats&		m_stats;

	Socket				m_socket;
	bool				m_compressionEnabled;
	CompressionData		m_compressionData;

						Connection(const Connection&) = delete;
	void				operator=(const Connection&) = delete;
};



///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

} // namespace eacopy
