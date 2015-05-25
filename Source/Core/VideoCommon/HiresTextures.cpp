// Copyright 2013 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <string>
#include <utility>
#include <xxhash.h>

#include "Common/CommonPaths.h"
#include "Common/FileSearch.h"
#include "Common/FileUtil.h"
#include "Common/StringUtil.h"

#include "Core/ConfigManager.h"

#include "VideoCommon/ImageLoader.h"
#include "VideoCommon/HiresTextures.h"
#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/TextureUtil.h"
#include "VideoCommon/VideoConfig.h"

typedef std::vector<std::pair<std::string, bool>> HiresTextureCacheItem;
typedef std::unordered_map<std::string, HiresTextureCacheItem> HiresTextureCache;
static HiresTextureCache s_textureMap;
static bool s_initialized = false;
static bool s_check_native_format;
static bool s_check_new_format;

static const std::string s_format_prefix = "tex1_";

void HiresTexture::Init(const std::string& gameCode, bool force_reload)
{
	if (!force_reload && s_initialized)
	{
		return;
	}
	s_initialized = true;
	s_check_native_format = false;
	s_check_new_format = false;
	s_textureMap.clear();
	CFileSearch::XStringVector Directories;
	std::string szDir = StringFromFormat("%s%s", File::GetUserPath(D_HIRESTEXTURES_IDX).c_str(), gameCode.c_str());
	Directories.push_back(szDir);

	for (u32 i = 0; i < Directories.size(); i++)
	{
		File::FSTEntry FST_Temp;
		File::ScanDirectoryTree(Directories[i], FST_Temp);
		for (auto& entry : FST_Temp.children)
		{
			if (entry.isDirectory)
			{
				bool duplicate = false;
				for (auto& Directory : Directories)
				{
					if (Directory == entry.physicalName)
					{
						duplicate = true;
						break;
					}
				}
				if (!duplicate)
					Directories.push_back(entry.physicalName);
			}
		}
	}
	std::string ddscode(".dds");
	std::string cddscode(".DDS");
	CFileSearch::XStringVector Extensions = {
		"*.png",
		"*.dds"
	};

	CFileSearch FileSearch(Extensions, Directories);
	const CFileSearch::XStringVector& rFilenames = FileSearch.GetFileNames();

	const std::string code = StringFromFormat("%s_", gameCode.c_str());
	const std::string miptag = "mip";
	if (rFilenames.size() > 0)
	{
		for (u32 i = 0; i < rFilenames.size(); i++)
		{
			std::string FileName;
			std::string Extension;			
			SplitPath(rFilenames[i], nullptr, &FileName, &Extension);
			bool newformat = false;
			bool dds_file = Extension.compare(ddscode) == 0 || Extension.compare(cddscode) == 0;
			if (FileName.substr(0, code.length()) == code)
			{
				s_check_native_format = true;				
			}
			else if (FileName.substr(0, s_format_prefix.length()) == s_format_prefix)
			{
				s_check_new_format = true;
				newformat = true;
			}
			else
			{
				// Discard wrong files
				continue;
			}
			std::pair<std::string, bool> Pair(rFilenames[i], dds_file);
			u32 level = 0;
			size_t idx = FileName.find_last_of('_');
			std::string miplevel = FileName.substr(idx + 1, std::string::npos);
			if (miplevel.substr(0, miptag.length()) == miptag)
			{
				sscanf(miplevel.substr(3, std::string::npos).c_str(), "%i", &level);
				FileName = FileName.substr(0, idx);
			}
			HiresTextureCache::iterator iter = s_textureMap.find(FileName);
			u32 min_item_size = level + 1;
			if (iter == s_textureMap.end())
			{
				HiresTextureCacheItem item(min_item_size);
				item[level] = Pair;
				s_textureMap.insert(HiresTextureCache::value_type(FileName, item));
			}
			else
			{
				if (iter->second.size() < min_item_size)
				{
					iter->second.resize(min_item_size);
				}
				if (iter->second[level].first.size() == 0)
				{
					iter->second[level] = Pair;
				}
			}
		}
	}
}

std::string HiresTexture::GenBaseName(
	const u8* texture, size_t texture_size,
	const u8* tlut, size_t tlut_size,
	u32 width, u32 height,
	int format,
	bool has_mipmaps,
	bool dump)
{
	std::string name = "";
	bool convert = false;
	HiresTextureCache::iterator convert_iter;
	if ((!dump || convert) && s_check_native_format)
	{
		// try to load the old format first
		u64 tex_hash = GetHashHiresTexture(texture, (int)texture_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);
		u64 tlut_hash = 0;
		if(tlut_size)
			tlut_hash = GetHashHiresTexture(tlut, (int)tlut_size, g_ActiveConfig.iSafeTextureCache_ColorSamples);
		name = StringFromFormat("%s_%08x_%i", SConfig::GetInstance().m_LocalCoreStartupParameter.m_strUniqueID.c_str(), (u32)(tex_hash ^ tlut_hash), (u16)format);
		convert_iter = s_textureMap.find(name);
		if (convert_iter != s_textureMap.end())
		{
			if (g_ActiveConfig.bConvertHiresTextures)
				convert = true;
			else
				return name;
		}
	}
	if (dump || s_check_new_format || convert)
	{
		// checking for min/max on paletted textures
		u32 min = 0xffff;
		u32 max = 0;
		switch (tlut_size)
		{
		case 0: break;
		case 16 * 2:
			for (size_t i = 0; i < texture_size; i++)
			{
				min = std::min<u32>(min, texture[i] & 0xf);
				min = std::min<u32>(min, texture[i] >> 4);
				max = std::max<u32>(max, texture[i] & 0xf);
				max = std::max<u32>(max, texture[i] >> 4);
			}
			break;
		case 256 * 2:
			for (size_t i = 0; i < texture_size; i++)
			{
				min = std::min<u32>(min, texture[i]);
				max = std::max<u32>(max, texture[i]);
			}
			break;
		case 16384 * 2:
			for (size_t i = 0; i < texture_size / 2; i++)
			{
				min = std::min<u32>(min, Common::swap16(((u16*)texture)[i]) & 0x3fff);
				max = std::max<u32>(max, Common::swap16(((u16*)texture)[i]) & 0x3fff);
			}
			break;
		}
		if (tlut_size > 0)
		{
			tlut_size = 2 * (max + 1 - min);
			tlut += 2 * min;
		}
		u64 tex_hash = XXH64(texture, texture_size);
		u64 tlut_hash = 0;
		if(tlut_size)
			tlut_hash = XXH64(tlut, tlut_size);
		std::string basename = s_format_prefix + StringFromFormat("%dx%d%s_%016" PRIx64, width, height, has_mipmaps ? "_m" : "", tex_hash);
		std::string tlutname = tlut_size ? StringFromFormat("_%016" PRIx64, tlut_hash) : "";
		std::string formatname = StringFromFormat("_%d", format);
		std::string fullname = basename + tlutname + formatname;
		if (convert)
		{
			// new texture
			if (s_textureMap.find(fullname) == s_textureMap.end())
			{
				HiresTextureCacheItem newitem;
				newitem.resize(convert_iter->second.size());
				for (int level = 0; level < convert_iter->second.size(); level++)
				{
					std::string newname = fullname;
					if (level)
						newname += StringFromFormat("_mip%d", level);
					newname += convert_iter->second[level].second;
					std::string &src = convert_iter->second[level].first;
					size_t postfix = src.find(name);
					std::string dst = src.substr(0, postfix) + newname;
					if (File::Rename(src, dst))
					{
						s_check_new_format = true;
						OSD::AddMessage(StringFromFormat("Rename custom texture %s to %s", src.c_str(), dst.c_str()), 5000);
					}
					else
					{
						ERROR_LOG(VIDEO, "rename failed");
					}
					newitem[level] = std::pair<std::string, bool>(dst, convert_iter->second[level].second);
				}
				s_textureMap.insert(HiresTextureCache::value_type(fullname, newitem));
			}
			else
			{
				for (int level = 0; level < convert_iter->second.size(); level++)
				{
					if (File::Delete(convert_iter->second[level].first))
					{
						OSD::AddMessage(StringFromFormat("Delete double old custom texture %s", convert_iter->second[level].first.c_str()), 5000);
					}
					else
					{
						ERROR_LOG(VIDEO, "delete failed");
					}
				}				
			}
			s_textureMap.erase(name);
		}
		return fullname;
	}
	return name;
}

inline void ReadPNG(ImageLoaderParams &ImgInfo)
{
	if (ImageLoader::ReadPNG(ImgInfo))
	{
		ImgInfo.resultTex = PC_TEX_FMT_RGBA32;
	}
}

inline void ReadDDS(ImageLoaderParams &ImgInfo)
{
	DDSCompression ddsc = ImageLoader::ReadDDS(ImgInfo);
	if (ddsc != DDSCompression::DDSC_NONE)
	{
		switch (ddsc)
		{
		case DDSC_DXT1:
			ImgInfo.resultTex = PC_TEX_FMT_DXT1;
			break;
		case DDSC_DXT3:
			ImgInfo.resultTex = PC_TEX_FMT_DXT3;
			break;
		case DDSC_DXT5:
			ImgInfo.resultTex = PC_TEX_FMT_DXT5;
			break;
		default:
			break;
		}
	}
}

HiresTexture* HiresTexture::Search(
	const std::string& basename,
	std::function<u8*(size_t)> request_buffer_delegate)
{
	if (s_textureMap.size() == 0)
	{
		return nullptr;
	}	
	HiresTextureCache::iterator iter = s_textureMap.find(basename);
	if (iter == s_textureMap.end())
	{
		return nullptr;
	}
	HiresTextureCacheItem& current = iter->second;
	if (current.size() == 0)
	{
		return nullptr;
	}
	// First level is mandatory
	if (current[0].first.size() == 0)
	{
		return nullptr;
	}
	HiresTexture* ret = nullptr;
	u8* buffer_pointer;
	u32 maxwidth;
	u32 maxheight;
	bool last_level_is_dds = false;
	for (size_t level = 0; level < current.size(); level++)
	{
		ImageLoaderParams imgInfo;
		std::pair<std::string, bool> &item = current[level];
		imgInfo.dst = nullptr;
		imgInfo.Path = item.first.c_str();
		if (level == 0)
		{
			imgInfo.request_buffer_delegate = [&](size_t requiredsize)
			{
				// Pre allocate space for the textures and potetially all the posible mip levels 
				return request_buffer_delegate((requiredsize * 4) / 3);
			};
		}
		else
		{
			imgInfo.request_buffer_delegate = [&](size_t requiredsize)
			{
				// just return the pointer to pack the textures in a single buffer.
				return buffer_pointer;
			};
		}
		bool ddsfile = false;
		if (item.second)
		{
			if (level > 0 && !last_level_is_dds)
			{
				// don't give support to mixed formats
				break;
			}
			ddsfile = true;
			last_level_is_dds = true;
			ReadDDS(imgInfo);
		}
		else
		{
			if (level > 0 && last_level_is_dds)
			{
				// don't give support to mixed formats
				break;
			}
			last_level_is_dds = false;
			ReadPNG(imgInfo);
		}
		if (imgInfo.dst == nullptr || imgInfo.resultTex == PC_TEX_FMT_NONE)
		{
			ERROR_LOG(VIDEO, "Custom texture %s failed to load level %i", imgInfo.Path, level);
			break;
		}
		if (level == 0)
		{
			ret = new HiresTexture();
			ret->m_format = imgInfo.resultTex;
			ret->m_width = maxwidth = imgInfo.Width;
			ret->m_height = maxheight = imgInfo.Height;
		}
		else
		{
			if (maxwidth != imgInfo.Width
				|| maxheight != imgInfo.Height
				|| ret->m_format != imgInfo.resultTex)
			{
				ERROR_LOG(VIDEO, 
					"Custom texture %s invalid level %i size: %i %i required: %i %i format: %i", 
					imgInfo.Path, 
					level, 
					imgInfo.Width, 
					imgInfo.Height, 
					maxwidth, 
					maxheight,
					ret->m_format);
				break;
			}
		}
		ret->m_levels++;
		if (ddsfile)
		{
			buffer_pointer = imgInfo.dst + TextureUtil::GetTextureSizeInBytes(maxwidth, maxheight, imgInfo.resultTex);
		}
		else
		{
			buffer_pointer = imgInfo.dst + imgInfo.data_size;
		}
		maxwidth = std::max(maxwidth >> 1, 1u);
		maxheight = std::max(maxheight >> 1, 1u);
		if (ddsfile &&  maxwidth < 4 && maxheight < 4)
		{
			return ret;
		}
		if (imgInfo.nummipmaps > 0)
		{
			// Give priority to load dds with packed levels
			ret->m_levels = imgInfo.nummipmaps + 1;
			break;
		}
	}
	return ret;
}