class sdllp
{
private:
	static std::map<std::string, HINSTANCE> libraries;

	static void load_library(const char* library);
	static bool	is_loaded(const char* library);

	static void log(const char* message, ...);
public:
	static FARPROC get_export(const char* function, const char* library);
};
