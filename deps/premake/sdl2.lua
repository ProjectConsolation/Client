sdl2 = {
	source = path.join(dependencies.basePath, "SDL"),
	fallback_source = path.join(dependencies.basePath, "SDL2-src/SDL2-2.32.10"),
}

function sdl2.resolve_source()
	if os.isdir(sdl2.source) then
		return sdl2.source
	end

	if os.isdir(sdl2.fallback_source) then
		return sdl2.fallback_source
	end

	return nil
end

function sdl2.available()
	return sdl2.resolve_source() ~= nil
end

function sdl2.collect_project_files()
	local files = {}
	local source = sdl2.resolve_source()

	if not source then
		return files
	end

	local project_file = path.join(source, "VisualC/SDL/SDL.vcxproj")
	for line in io.lines(project_file) do
		local compile_path = line:match('<ClCompile Include="([^"]+)"')
		if compile_path then
			compile_path = compile_path:gsub("\\", "/")
			compile_path = compile_path:gsub("^%.%./%.%./", "")
			table.insert(files, path.join(source, compile_path))
		end

		local resource_path = line:match('<ResourceCompile Include="([^"]+)"')
		if resource_path then
			resource_path = resource_path:gsub("\\", "/")
			resource_path = resource_path:gsub("^%.%./%.%./", "")
			table.insert(files, path.join(source, resource_path))
		end
	end

	-- Make headers visible in the project tree.
	-- The actual compile list still comes from SDL's own Visual Studio project.
	for _, header_glob in ipairs({
		path.join(source, "include/**.h"),
		path.join(source, "src/**.h"),
	}) do
		table.insert(files, header_glob)
	end

	return files
end

function sdl2.import()
	local source = sdl2.resolve_source()
	if not source then
		return
	end

	links {
		"SDL2-static",
		"setupapi",
		"winmm",
		"imm32",
		"version",
	}

	includedirs {
		path.join(source, "include"),
	}
end

function sdl2.project()
	local source = sdl2.resolve_source()
	if not source then
		return
	end

	project "SDL2-static"
		language "C"
		kind "StaticLib"

		files(sdl2.collect_project_files())

		includedirs {
			path.join(source, "include"),
		}

		resincludedirs {
			path.join(source, "include"),
		}

		defines {
			"SDL_STATIC_LIB",
			"HAVE_LIBC",
			"__WIN32__",
		}

		warnings "Off"
end

table.insert(dependencies, sdl2)
