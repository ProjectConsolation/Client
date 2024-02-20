
iw4of = {
	source = path.join(dependencies.basePath, "iw4-open-formats", "src"),
}

function iw4of.import()
	links { "iw4of" }
	iw4of.includes()
end

function iw4of.includes()
	includedirs {
		path.join(dependencies.basePath, "iw4-open-formats", "include")
	}
end

function iw4of.project()
	project "iw4of"
		warnings "Off"
		kind "StaticLib"
		language "C++"
		cppdialect "C++latest"

		pchheader "std_include.hpp"
		pchsource (path.join(iw4of.source, "iw4-of/std_include.cpp"))

		files {
			path.join(iw4of.source, "iw4-of/**.hpp"),
			path.join(iw4of.source, "iw4-of/**.cpp")
		}
		
		includedirs {
			path.join(iw4of.source, "iw4-of"),
			path.join(dependencies.basePath, "iw4-open-formats", "include")
		}

		libtomcrypt.import()
		libtommath.import()
		rapidjson.import()
		zlib.import()
		zstd.import()
end

table.insert(dependencies, iw4of)
