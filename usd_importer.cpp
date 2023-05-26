// Harfang Framework - Copyright 2001-2023 Thomas Simonnet. All Rights Reserved.
// USD format importer.

#define NOMINMAX

#include <engine/geometry.h>
#include <engine/model_builder.h>
#include <engine/node.h>
#include <engine/physics.h>
#include <engine/render_pipeline.h>
#include <engine/scene.h>
#include <engine/forward_pipeline.h>

#include <foundation/build_info.h>
#include <foundation/cext.h>
#include <foundation/cmd_line.h>
#include <foundation/dir.h>
#include <foundation/format.h>
#include <foundation/log.h>
#include <foundation/math.h>
#include <foundation/matrix3.h>
#include <foundation/matrix4.h>
#include <foundation/pack_float.h>
#include <foundation/path_tools.h>
#include <foundation/projection.h>
#include <foundation/sha1.h>
#include <foundation/string.h>
#include <foundation/time.h>
#include <foundation/vector3.h>
#include <engine/create_geometry.h>

#include <fstream>
#include <iostream>
#include <mutex>

#undef CopyFile
#undef GetObject

#include <foundation/file.h>

#include "pxr/usd/usd/stage.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usd/attribute.h"
#include "pxr/usd/usdGeom/mesh.h"
#include "pxr/usd/usdGeom/xformable.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/camera.h"
#include "pxr/usd/usdGeom/sphere.h"
#include "pxr/usd/usdShade/materialBindingAPI.h"
#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/ar/resolverContextBinder.h"
#include "pxr/usd/ar/asset.h"
#include "pxr/usd/ar/resolverContextBinder.h"
#include "pxr/usd/usdUtils/dependencies.h"
#include "pxr/usd/usd/primCompositionQuery.h"
#include "pxr/usd/usdLux/sphereLight.h"
#include "pxr/usd/usdLux/distantLight.h"
#include "pxr/usd/usdLux/domeLight.h"
#include "pxr/usd/usdHydra/tokens.h"
#include "pxr/usd/pcp/site.h"
#include "pxr/usd/pcp/layerStack.h"
#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/fileFormat.h"
#include "pxr/usd/ar/packageUtils.h"
#include "pxr/base/gf/vec3f.h"
#include "pTexture.h"

#include "json.hpp"
using nlohmann::json;

std::map<int, hg::NodeRef> idNode_to_NodeRef;
std::map<std::string, hg::TextureRef> picture_dest_path_to_tex_ref;
std::map<std::string, std::string> picture_sha1_to_dest_path;

struct AlreadySavedGeo {
	hg::Object object;
	std::vector<std::string> ids;
};
std::map<std::string, AlreadySavedGeo> already_saved_geo_with_primitives_ids;

static std::string Indent(const int indent) {
	std::string s;
	for (int i = 0; i < indent; i++) {
		s += "  ";
	}
	return s;
}

enum class ImportPolicy { SkipExisting, Overwrite, Rename, SkipAlways };

struct Config {
	ImportPolicy import_policy_geometry{ImportPolicy::SkipExisting}, import_policy_material{ImportPolicy::SkipExisting},
		import_policy_texture{ImportPolicy::SkipExisting}, import_policy_scene{ImportPolicy::SkipExisting}, import_policy_anim{ImportPolicy::SkipExisting};

	std::string input_path;
	std::string name; // output name (may be empty)
	std::string base_output_path{"./"};
	std::string prj_path;
	std::string prefix;
	std::string shader;

	float geometry_scale{1.f};
	int frame_per_second{24};

	bool import_animation{true};
	bool recalculate_normal{false}, recalculate_tangent{false};

	std::string finalizer_script;
};

static bool GetOutputPath(
	std::string &path, const std::string &base, const std::string &name, const std::string &prefix, const std::string &ext, ImportPolicy import_policy) {
	if (base.empty())
		return false;

	const auto filename = name.empty() ? prefix : (prefix.empty() ? name : prefix + "-" + name);
	path = hg::CleanPath(base + "/" + filename + "." + ext);

	// check folder exists and created
	hg::MkTree(hg::CutFileName(path).c_str());

	switch (import_policy) {
		default:
			return false;

		case ImportPolicy::SkipAlways:
			return false; // WARNING: Do not move this to the start of the function. The path for the resource is needed even if it is not exported.

		case ImportPolicy::SkipExisting:
			if (hg::Exists(path.c_str()))
				return false;
			break;

		case ImportPolicy::Overwrite:
			return true;

		case ImportPolicy::Rename:
			for (auto n = 0; hg::Exists(path.c_str()) && n < 10000; ++n) {
				std::ostringstream ss;
				ss << base << "/" << filename << "-" << std::setw(4) << std::setfill('0') << n << "." << ext;
				path = ss.str();
			}
			break;
	}
	return true;
}

//
static std::string MakeRelativeResourceName(const std::string &name, const std::string &base_path, const std::string &prefix) {
	if (hg::starts_with(name, base_path, hg::case_sensitivity::insensitive)) {
		const auto stripped_name = hg::lstrip(hg::slice(name, base_path.length()), "/");
		return prefix.empty() ? stripped_name : prefix + "/" + stripped_name;
	}
	return name;
}

//
static hg::Material ExportMaterial(const pxr::UsdShadeShader &shaderUSD, std::set<pxr::TfToken> &uvMapVarname, const pxr::UsdStage &stage,
	const Config &config, hg::PipelineResources &resources) {

	hg::debug(hg::format("	Exporting material '%1'").arg(shaderUSD.GetPath().GetString()));

	static const std::string meta_BC5_text("{\"profiles\": {\"default\": {\"compression\": \"BC5\"}}}");
	static const std::string meta_BC7_srgb_text("{\"profiles\": {\"default\": {\"compression\": \"BC7\", \"srgb\": 1}}}");

	//
	std::string dst_path;
	hg::Material mat;
	std::string shader("core/shader/pbr.hps");

	hg::Vec4 diffuse = {0.5f, 0.5f, 0.5f, 1.f}, orm = {1, 0, 0, 1}, emissive = {0, 0, 0, -1}, specular = {0.5f, 0.5f, 0.5f, 1.f}, ambient = {0, 0, 0, 1};

	hg::TextureRef albedoTexture, opacityTexture, occlusionTexture, roughnessTexture, metallicTexture;

	// get all inputs
	for (const auto &input : shaderUSD.GetInputs()) {
		auto attrs = input.GetValueProducingAttributes();
		if (attrs.size()) {
			auto baseNameShaderInput = input.GetAttr().GetBaseName().GetString();
			auto attr = attrs[0];
			auto outputShaderName = attr.GetBaseName().GetString();
			//auto y = attr.GetTypeName().GetCPPTypeName();

			// if there is a real value
			if (attr.HasAuthoredValue()) {
				if (baseNameShaderInput == "diffuseColor") {
					pxr::GfVec3f diffuseUSD;
					attr.Get(&diffuseUSD);
					diffuse.x = diffuseUSD.data()[0];
					diffuse.y = diffuseUSD.data()[1];
					diffuse.z = diffuseUSD.data()[2];
				} else if (baseNameShaderInput == "opacity") {
					attr.Get(&diffuse.w);
				} else if (baseNameShaderInput == "occlusion") {
					attr.Get(&orm.x);
				} else if (baseNameShaderInput == "roughness") {
					attr.Get(&orm.y);
				} else if (baseNameShaderInput == "metallic") {
					attr.Get(&orm.z);
				} else if (baseNameShaderInput == "emissiveColor") {
					pxr::GfVec3f emissiveUSD;
					attr.Get(&emissiveUSD);
					emissive.x = emissiveUSD.data()[0];
					emissive.y = emissiveUSD.data()[1];
					emissive.z = emissiveUSD.data()[2];
				}

			} else {
				pxr::UsdShadeShader shaderTexture(attr.GetPrim());
				pxr::ArResolverContextBinder resolverContextBinder(attr.GetPrim().GetStage()->GetPathResolverContext());

				pxr::TfToken shaderID;
				shaderTexture.GetShaderId(&shaderID);

				// it's a texture
				if (shaderID.GetString() == "UsdUVTexture") { // if (shaderID == pxr::UsdHydraTokens->HwUvTexture_1) { //|| shaderID == pxr::UsdHydraTokens->HwPtexTexture_1) {
					for (const auto &inputTexture : shaderTexture.GetInputs()) {
						auto baseNameTextureInput = inputTexture.GetBaseName().GetString();
						auto attrTexture = inputTexture.GetAttr();
						if (baseNameTextureInput == "file") {

							auto y = attrTexture.GetTypeName().GetCPPTypeName();

							// Retrieve the asset file.
							pxr::SdfAssetPath assetPath;
							attrTexture.Get(&assetPath);

							GetOutputPath(dst_path, config.base_output_path + "/Textures", hg::GetFileName(assetPath.GetAssetPath()), {}, hg::GetFileExtension(assetPath.GetAssetPath()), config.import_policy_texture);

							auto texRef = picture_dest_path_to_tex_ref[dst_path];

							// Add the texture to the material.
							if (baseNameShaderInput == "diffuseColor" && texRef != hg::InvalidTextureRef)
								albedoTexture = texRef;
							if (baseNameShaderInput == "opacity" && texRef != hg::InvalidTextureRef)
								opacityTexture = texRef;

							// Generate the ORM (Occlusion, Roughness, Metallic) using the available values.
							if (baseNameShaderInput == "occlusion" && texRef != hg::InvalidTextureRef)
								occlusionTexture = texRef;
							if (baseNameShaderInput == "roughness" && texRef != hg::InvalidTextureRef)
								roughnessTexture = texRef;
							if (baseNameShaderInput == "metallic" && texRef != hg::InvalidTextureRef)
								metallicTexture = texRef;

							// Handle the normal texture.
							if (baseNameShaderInput == "normal" && texRef != hg::InvalidTextureRef) {
								hg::debug(hg::format("		- uNormalMap: %1").arg(resources.textures.GetName(texRef)));

								if (GetOutputPath(dst_path, config.prj_path, resources.textures.GetName(texRef), {}, "meta", config.import_policy_texture)) {
									if (std::FILE *f = std::fopen(dst_path.c_str(), "w")) {
										std::fwrite(meta_BC5_text.data(), sizeof meta_BC5_text[0], meta_BC5_text.size(), f);
										std::fclose(f);
									}
								}
								mat.textures["uNormalMap"] = {texRef, 2};
							}

							// Handle the emissive texture.
							if (baseNameShaderInput == "emissiveColor" && texRef != hg::InvalidTextureRef) {
								hg::debug(hg::format("		- uSelfMap: %1").arg(resources.textures.GetName(texRef)));

								if (GetOutputPath(dst_path, config.prj_path, resources.textures.GetName(texRef), {}, "meta", config.import_policy_texture)) {
									if (std::FILE *f = std::fopen(dst_path.c_str(), "w")) {
										std::fwrite(meta_BC7_srgb_text.data(), sizeof meta_BC7_srgb_text[0], meta_BC7_srgb_text.size(), f);
										std::fclose(f);
									}
								}
								mat.textures["uSelfMap"] = {texRef, 4};
							}
						} else if (baseNameTextureInput == "st") {
							// Retrieve the source that is connected to the output.
							auto sourceUV = inputTexture.GetConnectedSources()[0].source;

							// Retrieve the shader where the output is located.
							pxr::UsdShadeShader shaderUV(sourceUV.GetPrim());

							// Retrieve the UV input.
							auto inputUVName = shaderUV.GetInput(pxr::TfToken("varname"));

							// If there's another connected source, update the inputUVName.
							if (inputUVName.GetConnectedSources().size() > 0) {
								auto UVNameSource = inputUVName.GetConnectedSources()[0].source;
								inputUVName = UVNameSource.GetInput(pxr::TfToken("stPrimvarName"));
							}

							// Retrieve the token reference within the geometry.
							pxr::TfToken UVName;
							inputUVName.GetAttr().Get(&UVName);
							uvMapVarname.insert(UVName);
						}
					}
				}
			}
		} else {
			hg::error(hg::format("!!! Can't find attr for %1").arg(input.GetFullName().GetString()));
		}
	}

	// Check if there is an albedo.
	if (albedoTexture != hg::InvalidTextureRef) {
		hg::debug(hg::format("		- uBaseOpacityMap: %1").arg(resources.textures.GetName(albedoTexture)));

		json preprocess;
		if (opacityTexture != hg::InvalidTextureRef) {
			preprocess = {"preprocess", {{"construct", {"R", "G", "B", {{"path", resources.textures.GetName(opacityTexture)}, {"channel", "A"}}}}}};
		}

		json meta_albedoTexture_json = {{"profiles", {{"default", {{"compression", "BC7"}, preprocess}}}}};
		auto meta_albedoTexture = meta_albedoTexture_json.dump();

		if (GetOutputPath(dst_path, config.prj_path, resources.textures.GetName(albedoTexture), {}, "meta", config.import_policy_texture)) {
			if (std::FILE *f = std::fopen(dst_path.c_str(), "w")) {
				std::fwrite(meta_albedoTexture.data(), sizeof meta_albedoTexture[0], meta_albedoTexture.size(), f);
				std::fclose(f);
			}
		}

		mat.textures["uBaseOpacityMap"] = {albedoTexture, 0};
	}else // Check for cases where there is no albedo texture, but only opacity (e.g., for decals).
		if (opacityTexture != hg::InvalidTextureRef) { 
		hg::debug(hg::format("		- uOpacityMap: %1").arg(resources.textures.GetName(opacityTexture)));

		json meta_opacityTexture_json = {{"profiles", {{"default", {{"compression", "BC7"}, {"preprocess", {{"construct", {(int)(diffuse.x * 255), (int)(diffuse.y * 255), (int)(diffuse.z * 255), {{"path", resources.textures.GetName(opacityTexture)}, {"channel", "A"}}}}}}}}}}};
		auto meta_opacityTexture = meta_opacityTexture_json.dump();

		if (GetOutputPath(dst_path, config.prj_path, resources.textures.GetName(opacityTexture), {}, "meta", config.import_policy_texture)) {
			if (std::FILE *f = std::fopen(dst_path.c_str(), "w")) {
				std::fwrite(meta_opacityTexture.data(), sizeof meta_opacityTexture[0], meta_opacityTexture.size(), f);
				std::fclose(f);
			}
		}

		mat.textures["uBaseOpacityMap"] = {opacityTexture, 0};
	}

	// Check if there is an ORM (Occlusion, Roughness, Metallic).
	if (occlusionTexture != hg::InvalidTextureRef || roughnessTexture != hg::InvalidTextureRef || metallicTexture != hg::InvalidTextureRef) {
		json preprocess;

		auto metallicRoughnessTexture =
			occlusionTexture != hg::InvalidTextureRef ? occlusionTexture : (roughnessTexture != hg::InvalidTextureRef ? roughnessTexture : metallicTexture);

		if (occlusionTexture != roughnessTexture || roughnessTexture != metallicTexture || metallicTexture != occlusionTexture) {
			json occlusionJSON;
			if (occlusionTexture != hg::InvalidTextureRef)
				occlusionJSON = resources.textures.GetName(occlusionTexture);
			else
				occlusionJSON = (int)orm.x * 255;
			json roughnessJSON;
			if (roughnessTexture != hg::InvalidTextureRef)
				roughnessJSON = resources.textures.GetName(roughnessTexture);
			else
				roughnessJSON = (int)orm.y * 255;
			json metallicJSON;
			if (metallicTexture != hg::InvalidTextureRef)
				metallicJSON = resources.textures.GetName(metallicTexture);
			else
				metallicJSON = (int)orm.z * 255;
			preprocess = {"preprocess", {{"construct", {occlusionJSON, roughnessJSON, metallicJSON}}}};
		}

		json meta_occlusionTexture_json = {{"profiles", {{"default", {{"compression", "BC7"}, preprocess}}}}};
		auto meta_occlusionTexture = meta_occlusionTexture_json.dump();

		if (GetOutputPath(dst_path, config.prj_path, resources.textures.GetName(metallicRoughnessTexture), {}, "meta", config.import_policy_texture)) {
			if (std::FILE *f = std::fopen(dst_path.c_str(), "w")) {
				std::fwrite(meta_occlusionTexture.data(), sizeof meta_occlusionTexture[0], meta_occlusionTexture.size(), f);
				std::fclose(f);
			}
		}
		mat.textures["uOcclusionRoughnessMetalnessMap"] = {metallicRoughnessTexture, 1};
	}

	mat.values["uBaseOpacityColor"] = {bgfx::UniformType::Vec4, {diffuse.x, diffuse.y, diffuse.z, diffuse.w}};
	mat.values["uOcclusionRoughnessMetalnessColor"] = {bgfx::UniformType::Vec4, {orm.x, orm.y, orm.z, orm.w}};
	mat.values["uSelfColor"] = {bgfx::UniformType::Vec4, {emissive.x, emissive.y, emissive.z, emissive.w}};

	if (opacityTexture != hg::InvalidTextureRef || diffuse.w < 1)
		SetMaterialBlendMode(mat, hg::BM_Alpha);

	if (!config.shader.empty())
		shader = config.shader; // Use the overridden shader if provided.

	hg::debug(hg::format("		- Using pipeline shader '%1'").arg(shader));
	mat.program = resources.programs.Add(shader.c_str(), {});

	// FinalizeMaterial(mat, fbx_material->GetName(), geo_name);
	return mat;
}

#define __PolIndex (pol_index[p] + v)
#define __PolRemapIndex (pol_index[p] + (geo.pol[p].vtx_count - 1 - v))

static void ExportGeometry(
	const pxr::UsdGeomMesh &geoMesh, const pxr::UsdGeomSubset *geoMeshSubSet, hg::Geometry &geo, const std::set<pxr::TfToken> &uvMapVarname) {
	pxr::VtArray<pxr::GfVec3f> points;
	pxr::VtArray<pxr::GfVec3f> normals;
	std::vector<pxr::VtArray<pxr::GfVec2f>> uvs;
	pxr::VtArray<int> faceVertexCounts;
	pxr::VtArray<int> faceVertexIndices;
	pxr::VtArray<int> faceSubsetIndices;

	// vertices
	geoMesh.GetPointsAttr().Get(&points);
	geo.vtx.resize(points.size());
	memcpy(geo.vtx.data(), points.data(), geo.vtx.size() * sizeof(float) * 3);

	// apply global scale from usd to be in meter
	const auto globalScale = pxr::UsdGeomGetStageMetersPerUnit(geoMesh.GetPrim().GetStage());
	for (auto &v : geo.vtx) {
		v *= globalScale;
	}

	// normals
	geoMesh.GetNormalsAttr().Get(&normals);

	// faceVertexCounts
	geoMesh.GetFaceVertexCountsAttr().Get(&faceVertexCounts);

	// faceVertexIndices
	geoMesh.GetFaceVertexIndicesAttr().Get(&faceVertexIndices);

	// uv texcoord from blender (TODO test from other sources)
	for (const auto &UVToken : uvMapVarname) {
		auto UVPrim = pxr::UsdGeomPrimvar(geoMesh.GetPrim().GetAttribute(pxr::TfToken("primvars:"+UVToken.GetString())));
		if (UVPrim.HasValue()) {
			uvs.resize(uvs.size()+1);			
			UVPrim.Get(&uvs.back());
		}
	}
	// If a geometry subset exists, retrieve its indices.
	if (geoMeshSubSet)
		geoMeshSubSet->GetIndicesAttr().Get(&faceSubsetIndices);


	hg::debug(hg::format("	%1: geoMesh.points = %2\n").arg(__func__).arg(points.size()));
	hg::debug(hg::format("		# of normals = %1\n").arg(normals.size()));
	hg::debug(hg::format("		# of faceVertexCounts = %1\n").arg(faceVertexCounts.size()));
	hg::debug(hg::format("		# of faceVertexIndices = %1\n").arg(faceVertexIndices.size()));
	hg::debug(hg::format("		# of  nb uv = %1\n").arg(uvs.size()));
	hg::debug(hg::format("		# of faceSubsetIndices = %1\n").arg(faceSubsetIndices.size()));

	size_t face_offset = 0;
	for (size_t fid = 0; fid < faceVertexCounts.size(); fid++) {
		int f_count = faceVertexCounts[fid];

		assert(f_count >= 3);

		hg::Geometry::Polygon p{uint8_t(f_count), 0};
		geo.pol.push_back(p);

		for (size_t f = 0; f < f_count; f++) {
			// indices
			geo.binding.push_back(faceVertexIndices[face_offset + (f_count-1-f)]);

			// normal x,y,z
			if (normals.size()) {
				int idx = face_offset + (f_count - 1 - f);
				if (normals.size() == points.size())
					idx = faceVertexIndices[face_offset + (f_count - 1 - f)];

				hg::Vec3 n(normals[idx][0], normals[idx][1], normals[idx][2]);						
					
				geo.normal.push_back(n);
			}

			// u, v
			for (int i = 0; i < uvs.size(); ++i) {
				const auto &uvUSD = uvs[i];
				int idx = face_offset + (f_count - 1 - f);
				if (normals.size() == points.size())
					idx = faceVertexIndices[face_offset + (f_count - 1 - f)];
			
				hg::Vec2 uv(uvUSD[idx][0], uvUSD[idx][1]);
				uv.y = 1.f - uv.y;
				geo.uv[i].push_back(uv);
			}
		}
		face_offset += f_count;
	}

	// If a subset exists, modify the geometry. TODO: This current method is not very efficient, consider optimization.
	if (faceSubsetIndices.size() > 0) {
		std::vector<hg::Geometry::Polygon> pol;
		std::vector<uint32_t> binding;
		std::vector<hg::Vec3> normal; // per-polygon-vertex

		std::array<hg::Geometry::UVSet, 8> uv; // per-polygon-vertex

		size_t face_offset = 0;
		for (size_t i = 0; i < geo.pol.size(); ++i) {
			// find if this poly is in the mesh
			for (const auto &j: faceSubsetIndices)
				if (i == j) {
					pol.push_back(geo.pol[i]);
					for (size_t f = 0; f < geo.pol[i].vtx_count; f++) {
						// indices
						binding.push_back(geo.binding[face_offset + f]);

						// normal x,y,z
						if (normals.size())
							normal.push_back(geo.normal[face_offset + f]);

						// u, v
						for (int i = 0; i < uvs.size(); ++i)
							uv[i].push_back(geo.uv[i][face_offset + f]);
					}

					break;
				}

			face_offset += geo.pol[i].vtx_count;
		}
		geo.pol = pol;
		geo.binding = binding;
		geo.normal = normal;
		geo.uv = uv;
	}
}

static hg::Object GetObjectWithMaterial(const pxr::UsdPrim &p, std::set<pxr::TfToken> &uvMapVarname, hg::Scene &scene,
	const Config &config, hg::PipelineResources &resources) {

	pxr::UsdGeomMesh geoUSD(p);

	std::string path = p.GetPath().GetString();
	auto object = scene.CreateObject();

	// Add material to the primitive.
	// MATERIALS:
	// Assign one material per primitive.
	bool foundMat = false;
	pxr::UsdShadeMaterialBindingAPI materialBinding(p);
	auto binding = materialBinding.GetDirectBinding();
	if (pxr::UsdShadeMaterial shadeMaterial = binding.GetMaterial()) {
		pxr::UsdShadeShader shader = shadeMaterial.ComputeSurfaceSource();

		// if there is no shader with defaut render context, find the ONE
		if (!shader) {
			// find the output surface with the UsdPreviewSurface (we handle this one for now)
			auto outputs = shadeMaterial.GetSurfaceOutputs();
			for (const auto &output : outputs) {
				if (output.HasConnectedSource()) {
					// get the source connected to the output
					auto sourceOutput = output.GetConnectedSources()[0].source;
					auto sourceShaderName = sourceOutput.GetPrim().GetName().GetString();
					if (sourceShaderName == "UsdPreviewSurface")
						shader = pxr::UsdShadeShader(sourceOutput.GetPrim());
				}
			}
		}

		if (shader) {
			foundMat = true;

			// get the material
			auto mat = ExportMaterial(shader, uvMapVarname, *p.GetStage(), config, resources);
			/*
			if (geo.skin.size())
				mat.flags |= hg::MF_EnableSkinning;
			*/

			// check double side
			bool isDoubleSided = false;
			geoUSD.GetDoubleSidedAttr().Get(&isDoubleSided);
			// if it's a geo subset check the parent 
			if (p.GetTypeName() == "GeomSubset")
				pxr::UsdGeomMesh(p.GetParent()).GetDoubleSidedAttr().Get(&isDoubleSided);

			if (isDoubleSided)
				SetMaterialFaceCulling(mat, hg::FC_Disabled);

			object.SetMaterial(0, std::move(mat));
			object.SetMaterialName(0, shader.GetPath().GetString());
		}else
			hg::error("!Unexpected shader from UsdShadeShader()");
	}
	
	// If the material is not found, create a dummy material to make the object visible in the engine.
	if(!foundMat) {
		hg::debug(hg::format("	- Has no material, set a dummy one"));

		hg::Material mat;
		std::string shader;

		shader = "core/shader/pbr.hps";

		if (!config.shader.empty())
			shader = config.shader; // Use the overridden shader if it is provided in the configuration.

		hg::debug(hg::format("	- Using pipeline shader '%1'").arg(shader));
		mat.program = resources.programs.Add(shader.c_str(), {});

		// check in case there is special primvars
		hg::Vec4 diffuse = {0.5f, 0.5f, 0.5f, 1.f};
		if (auto diffuseAttr = p.GetAttribute(pxr::TfToken("primvars:displayColor"))) {
			//auto y = diffuseAttr.GetTypeName().GetCPPTypeName();
			pxr::VtArray<pxr::GfVec3f> diffuseUSD;
			diffuseAttr.Get(&diffuseUSD);
			if (diffuseUSD.size() > 1) {
				diffuse.x = diffuseUSD[0].data()[0];
				diffuse.y = diffuseUSD[0].data()[1];
				diffuse.z = diffuseUSD[0].data()[2];
			}
		}

		mat.values["uBaseOpacityColor"] = {bgfx::UniformType::Vec4, {diffuse.x, diffuse.y, diffuse.z, diffuse.w}};
		mat.values["uOcclusionRoughnessMetalnessColor"] = {bgfx::UniformType::Vec4, {1.f, 1.f, 0.f, -1.f}};
		mat.values["uSelfColor"] = {bgfx::UniformType::Vec4, {0.f, 0.f, 0.f, -1.f}};

		object.SetMaterial(0, std::move(mat));
		object.SetMaterialName(0, "dummy_mat");
	}

	return object;
}
std::map<std::string, hg::Object> primToObject;
std::map<std::string, std::string> protoToInstance;
//
static hg::Object ExportObject(const pxr::UsdPrim &p, hg::Node *node, hg::Scene &scene, const Config &config, hg::PipelineResources &resources) {
	hg::Object object;

	pxr::UsdGeomMesh geoUSD(p);
	std::string path = p.GetPath().GetString();

	std::string hashIdentifierPrim;
	for (auto o : p.GetPrimIndex().GetNodeRange())	
		hashIdentifierPrim = pxr::TfStringify(o.GetLayerStack()) + o.GetPath().GetText();

	//auto j = p.GetPrimIndex().DumpToString();
	//auto d = p.GetPrimIndex().GetNodeRange().DumpToString();
	/*auto m = pxr::TfStringify(p.GetPrimIndex().GetRootNode().GetLayerStack());
	auto j=p.GetPrimIndex().GetRootNode().GetLayerStack()->GetLayers();
	auto k = p.GetPrimIndex().GetRootNode().GetLayerStack()->GetLayers()[0]->GetDisplayName();
*/
	// If the geometry is not found, import it.
	if (primToObject.find(hashIdentifierPrim) != primToObject.end()) {
		object = primToObject[hashIdentifierPrim];
	} else {
		// If the geometry is not found, import it.
		hg::Geometry geo;

		std::set<pxr::TfToken> uvMapVarname;

		object = GetObjectWithMaterial(p, uvMapVarname, scene, config, resources);

		ExportGeometry(geoUSD, nullptr, geo, uvMapVarname);

		const auto vtx_to_pol = hg::ComputeVertexToPolygon(geo);
		auto vtx_normal = hg::ComputeVertexNormal(geo, vtx_to_pol, hg::Deg(45.f));

		// Recalculate the vertex normals.
		bool recalculate_normal = config.recalculate_normal;
		if (geo.normal.empty())
			recalculate_normal = true;

		if (recalculate_normal) {
			hg::debug("    - Recalculate normals");
			geo.normal = vtx_normal;
		} else
			vtx_normal = geo.normal;

		// Recalculate the vertex tangent frame.
		bool recalculate_tangent = config.recalculate_tangent;
		if (geo.tangent.empty())
			recalculate_tangent = true;
		else if (geo.tangent.size() != geo.normal.size()) { // be sure tangent is same size of normal, some strange things can happen with multiple
			hg::debug("CAREFUL Normal and Tangent are not the same size, can happen if you have submesh (some with tangent and some without)");
			geo.tangent.resize(geo.normal.size());
		}

		if (recalculate_tangent) {
			hg::debug("    - Recalculate tangent frames (MikkT)");
			if (!geo.uv[0].empty())
				geo.tangent = hg::ComputeVertexTangent(geo, vtx_normal, 0, hg::Deg(45.f));
		}

		if (GetOutputPath(path, config.base_output_path, path, {}, "geo", config.import_policy_geometry)) {
			hg::debug(hg::format("Export geometry to '%1'").arg(path));
			hg::SaveGeometryToFile(path.c_str(), geo);
		}

		path = MakeRelativeResourceName(path, config.prj_path, config.prefix);

		object.SetModelRef(resources.models.Add(path.c_str(), {}));

		primToObject[hashIdentifierPrim] = object;
	}	

		// find bind pose in the skins
	/*	if (gltf_node.skin >= 0) {
			hg::debug(hg::format("Exporting geometry skin"));

			const auto &skin = model.skins[gltf_node.skin];
			geo.bind_pose.resize(skin.joints.size());

			const auto attribAccessor = model.accessors[skin.inverseBindMatrices];
			const auto &bufferView = model.bufferViews[attribAccessor.bufferView];
			const auto &buffer = model.buffers[bufferView.buffer];
			const auto dataPtr = buffer.data.data() + bufferView.byteOffset + attribAccessor.byteOffset;
			const auto byte_stride = attribAccessor.ByteStride(bufferView);
			const auto count = attribAccessor.count;

			switch (attribAccessor.type) {
				case TINYGLTF_TYPE_MAT4: {
					switch (attribAccessor.componentType) {
						case TINYGLTF_COMPONENT_TYPE_DOUBLE:
						case TINYGLTF_COMPONENT_TYPE_FLOAT: {
							floatArray<float> value(arrayAdapter<float>(dataPtr, count * 16, sizeof(float)));

							for (size_t k{0}; k < count; ++k) {
								hg::Mat4 m_InverseBindMatrices(value[k * 16], value[k * 16 + 1], value[k * 16 + 2], value[k * 16 + 4], value[k * 16 + 5],
									value[k * 16 + 6], value[k * 16 + 8], value[k * 16 + 9], value[k * 16 + 10], value[k * 16 + 12], value[k * 16 + 13],
									value[k * 16 + 14]);

								m_InverseBindMatrices = hg::InverseFast(m_InverseBindMatrices);

								auto p = hg::GetT(m_InverseBindMatrices);
								p.z = -p.z;
								auto r = hg::GetR(m_InverseBindMatrices);
								r.x = -r.x;
								r.y = -r.y;
								auto s = hg::GetS(m_InverseBindMatrices);

								geo.bind_pose[k] = hg::InverseFast(hg::TransformationMat4(p, r, s));
							}
						} break;
						default:
							hg::error("Unhandeled component type for inverseBindMatrices");
					}
				} break;
				default:
					hg::error("Unhandeled MAT4 type for inverseBindMatrices");
			}
		}*/

	return object;

}

static void ExportCamera(const pxr::UsdPrim &p, hg::Node *nodeParent, hg::Scene &scene, const Config &config, hg::PipelineResources &resources) {
	auto camera = scene.CreateCamera();
	nodeParent->SetCamera(camera);

	auto cameraUSD = pxr::UsdGeomCamera(p);

	pxr::GfVec2f clippingRange;
	cameraUSD.GetClippingRangeAttr().Get(&clippingRange);
	camera.SetZNear(clippingRange[0]);
	camera.SetZFar(clippingRange[1]);

	pxr::TfToken projAttr;
	cameraUSD.GetProjectionAttr().Get(&projAttr);
	if (projAttr == pxr::UsdGeomTokens->orthographic) {
		camera.SetIsOrthographic(true);
	}else {
		float fov;
		cameraUSD.GetVerticalApertureAttr().Get(&fov);
		camera.SetFov(hg::Deg(fov));
		camera.SetIsOrthographic(false);
	}	
}

static void ExportLight(const pxr::UsdPrim &p, const pxr::TfToken type, hg::Node *nodeParent, hg::Scene &scene, const Config &config, hg::PipelineResources &resources) {
	auto light = scene.CreateLight();
	nodeParent->SetLight(light);
	pxr::UsdLuxBoundableLightBase lightUSD;
	if (type == "SphereLight") {
		pxr::UsdLuxSphereLight sphereLight(p);
		lightUSD = (pxr::UsdLuxBoundableLightBase)sphereLight;
		light.SetType(hg::LT_Point);

		float radiusAttr;
		sphereLight.GetRadiusAttr().Get(&radiusAttr);
		light.SetRadius(radiusAttr);

	} else if (type == "DistantLight") {
		pxr::UsdLuxDistantLight distantLight(p);
		lightUSD = (pxr::UsdLuxBoundableLightBase)distantLight;
		light.SetType(hg::LT_Spot);

		float angleAttr;
		distantLight.GetAngleAttr().Get(&angleAttr);
		light.SetRadius(angleAttr);

	} else if (type == "DomeLight") {
		pxr::UsdLuxDomeLight domeLight(p);
		lightUSD = (pxr::UsdLuxBoundableLightBase)domeLight;
	}	
	
	// add common value
	if (type == "SphereLight" || type == "DistantLight" || type == "DomeLight") {
		pxr::GfVec3f colorAttr;
		lightUSD.GetColorAttr().Get(&colorAttr);
		light.SetDiffuseColor(hg::Color(colorAttr.data()[0], colorAttr.data()[1], colorAttr.data()[2]));
	}		
}

static hg::Mat4 GetXFormMat(const pxr::UsdPrim& p) {
	auto xForm = pxr::UsdGeomXformable(p);
	pxr::GfMatrix4d transform;
	bool resetsXformStack;
	xForm.GetLocalTransformation(&transform, &resetsXformStack);

	hg::Mat4 m(transform.data()[0], transform.data()[1], transform.data()[2], transform.data()[4], transform.data()[5], transform.data()[6], transform.data()[8],
		transform.data()[9], transform.data()[10], transform.data()[12], transform.data()[13], transform.data()[14]);

	auto t = hg::GetT(m) * pxr::UsdGeomGetStageMetersPerUnit(p.GetStage());
	hg::SetT(m, t);

	return m;
}

//
static void ExportNode(const pxr::UsdPrim &p, hg::Node *nodeParent, hg::Scene &scene, const Config &config, hg::PipelineResources &resources) {

	auto type = p.GetTypeName();

	if (type == "Material" || type == "Shader") // don't export node to scene for these types
		return;

	hg::log(hg::format("type: %1, %2").arg(type.GetString()).arg(p.GetPath().GetString().c_str()));
	pxr::ArResolverContextBinder resolverContextBinder(p.GetStage()->GetPathResolverContext());

	auto node = scene.CreateNode(p.GetName());
	node.SetTransform(scene.CreateTransform());

	// Transform
	hg::Mat4 m = GetXFormMat(p);

	// If there is no parent, modify the base matrix.
	if (!nodeParent) {
		// Rotate the transform to account for the Z-axis as the up direction.
		if (UsdGeomGetStageUpAxis(p.GetStage()) == pxr::UsdGeomTokens->z) {
			hg::Mat44 to_hg(1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f);

			auto xForm = pxr::UsdGeomXformable(p);
			pxr::GfMatrix4d transform;
			bool resetsXformStack;
			xForm.GetLocalTransformation(&transform, &resetsXformStack);

			hg::Mat44 m44(transform.data()[0], transform.data()[1], transform.data()[2], transform.data()[3], transform.data()[4], transform.data()[5],
				transform.data()[6], transform.data()[7], transform.data()[8], transform.data()[9], transform.data()[10], transform.data()[11],
				transform.data()[12], transform.data()[13], transform.data()[14], transform.data()[15]);

			m44 = to_hg * m44;

			hg::Mat4 mt(m44.m[0][0], m44.m[1][0], m44.m[2][0], m44.m[0][1], m44.m[1][1], m44.m[2][1], m44.m[0][2], m44.m[1][2], m44.m[2][2], m44.m[0][3],
				m44.m[1][3], m44.m[2][3]);
			m = mt;
		}
		auto s = hg::GetS(m) * config.geometry_scale;
		hg::SetS(m, s);
	} else {
		// there is a node parent, so parent it
		node.GetTransform().SetParent(nodeParent->ref);
	}

	// Camera
	if (type == "Camera") {
		m = m * hg::Mat4(hg::RotationMatX(hg::Pi)) * hg::Mat4(hg::RotationMatZ(hg::Pi));
		auto s = hg::GetS(m);
		hg::SetS(m, hg::Vec3(-s.x, s.y, s.z));
		ExportCamera(p, &node, scene, config, resources);
	}// light
	else if (type == "DomeLight" || type == "DistantLight" || type == "SphereLight") {
		ExportLight(p, type, &node, scene, config, resources);
	}// Mesh 
	else if (type == "Mesh") {
		auto object = ExportObject(p, &node, scene, config, resources);
		// set object
		node.SetObject(object);
	}// GeomSubset
	else if (type == "GeomSubset") {
		hg::Object object;

		std::string hashIdentifierPrim;
		for (auto o : p.GetPrimIndex().GetNodeRange())
			hashIdentifierPrim = pxr::TfStringify(o.GetLayerStack()) + o.GetPath().GetText();

		// auto j = c.GetPrimIndex().DumpToString();
		// If the geometry is not found, import it.
		if (primToObject.find(hashIdentifierPrim) != primToObject.end()) {
			object = primToObject[hashIdentifierPrim];
		} else {
			std::string path = p.GetPath().GetString();
			hg::debug(hg::format("	add geometry subset").arg(path));
			pxr::UsdGeomSubset subsetC(p);
			std::set<pxr::TfToken> uvMapVarname;
			object = GetObjectWithMaterial(p, uvMapVarname, scene, config, resources);

			hg::Geometry geoSubset;
			pxr::UsdGeomMesh geoUSDParent(p.GetParent());
			ExportGeometry(geoUSDParent, &subsetC, geoSubset, uvMapVarname);

			if (GetOutputPath(path, config.base_output_path, path, {}, "geo", config.import_policy_geometry)) {
				hg::debug(hg::format("Export geometry to '%1'").arg(path));
				hg::SaveGeometryToFile(path.c_str(), geoSubset);
			}

			path = MakeRelativeResourceName(path, config.prj_path, config.prefix);
			object.SetModelRef(resources.models.Add(path.c_str(), {}));
			primToObject[hashIdentifierPrim] = object;
		}
		node.SetObject(object);

		// If it's a subset, make sure to remove the parent mesh object.
		nodeParent->SetObject({});

	} // Sphere
	else if (type == "Sphere") {
		std::set<pxr::TfToken> uvMapVarname;
		auto object = GetObjectWithMaterial(p, uvMapVarname, scene, config, resources);

		// FIXME: Unable to save procedural geometry, replace it with a generic model.
	/* bgfx::VertexLayout vs_pos_normal_decl;
		vs_pos_normal_decl.begin();
		vs_pos_normal_decl.add(bgfx::Attrib::Enum::Position, 3, bgfx::AttribType::Enum::Float);
		vs_pos_normal_decl.add(bgfx::Attrib::Enum::Normal, 3, bgfx::AttribType::Enum::Uint8, true, true);
		vs_pos_normal_decl.end();
		*/
		pxr::UsdGeomSphere sphere(p);
		float radiusAttr = 1.f;
		sphere.GetRadiusAttr().Get(&radiusAttr);
		m = m * hg::ScaleMat4(radiusAttr * pxr::UsdGeomGetStageMetersPerUnit(p.GetStage()));

		/* auto sphere_model = hg::CreateSphereModel(vs_pos_normal_decl, radiusAttr, 5, 5);
		auto sphere_model_ref = resources.models.Add(p.GetPath().GetString().c_str(), sphere_model);
		object.SetModelRef(sphere_model_ref);
		*/
		object.SetModelRef(resources.models.Add("core_library/primitives/sphere.geo", {}));
		node.SetObject(object);

	}
	
	// Check the children.
	if (p.IsInstance()){
		auto proto = p.GetPrototype();
		auto protoName = proto.GetName().GetString();
		std::string out_path_proto;
		if (protoToInstance.find(protoName) != protoToInstance.end()) {
			out_path_proto = protoToInstance[protoName];
		} else {
			hg::Scene sceneProto;
			auto nodeProto = sceneProto.CreateNode(protoName);
			nodeProto.SetTransform(sceneProto.CreateTransform());

			for (auto c : p.GetPrototype().GetChildren())
				ExportNode(c, &nodeProto, sceneProto, config, resources);

			nodeProto.GetTransform().SetParent(node.ref);

			if (GetOutputPath(out_path_proto, config.base_output_path, protoName, {}, "scn", config.import_policy_scene))
				SaveSceneJsonToFile(out_path_proto.c_str(), sceneProto, resources);

			out_path_proto = MakeRelativeResourceName(out_path_proto, config.prj_path, config.prefix);
			protoToInstance[protoName] = out_path_proto;
		}

		node.SetInstance(scene.CreateInstance(out_path_proto));
	}
	else
		for (auto c : p.GetChildren())
			ExportNode(c, &node, scene, config, resources);

	// Set the matrix
	node.GetTransform().SetLocal(m);
}

static bool ImportUSDScene(const std::string &path, const Config &config) {
	const auto t_start = hg::time_now();

	if (config.base_output_path.empty())
		return false;
	// create output directory if missing
	if (hg::Exists(config.base_output_path.c_str())) {
		if (!hg::IsDir(config.base_output_path.c_str()))
			return false; // can't output to this path
	} else {
		if (!hg::MkDir(config.base_output_path.c_str()))
			return false;
	}
	// create texture directory if missing
	if (!hg::Exists((config.base_output_path + "/Textures").c_str()))
		hg::MkDir((config.base_output_path + "/Textures").c_str());

	hg::Scene scene;
	hg::PipelineResources resources;

	//
	auto stage = pxr::UsdStage::Open(path);
	//auto stage = pxr::UsdStage::Open("C:\\boulot\\works\\Harfang\\couch.usda");
	//auto stage = pxr::UsdStage::Open("C:\\Users\\Scorpheus\\Downloads\\island-usd-v2.0\\island-usd-v2.0\\island\\usd\\elements\\isBayCedarA1\\element.usda");
	
	pxr::ArResolverContextBinder resolverContextBinder(stage->GetPathResolverContext());

/* 	std::string flattenStateString;
	stage->Flatten()->ExportToString(&flattenStateString);
	
	std::ofstream file("C:/boulot/works/Harfang/export_test_flatten.usda");
	file << flattenStateString;
	file.close();
	*/
	//stage = pxr::UsdStage::Open(stage->Flatten());

	//std::vector<pxr::SdfLayerRefPtr> layers;
	//std::vector<std::string> assets;
	//std::vector<std::string> unresolvedPaths;
	//pxr::UsdUtilsComputeAllDependencies(pxr::SdfAssetPath(path), &layers, &assets, &unresolvedPaths);

	// save all textures
	for (const auto &p : stage->TraverseAll()) {
		// look for usdUvTexture in all prim
		if (pxr::UsdAttribute attr = p.GetAttribute(pxr::UsdShadeTokens->infoId)) {
			pxr::TfToken infoId;
			attr.Get(&infoId);
			if (infoId.GetString() == "UsdUVTexture") {//infoId == pxr::UsdHydraTokens->HwUvTexture_1) { // || infoId == pxr::UsdHydraTokens->HwPtexTexture_1) {
				// look for the filename
				pxr::UsdShadeShader shaderTexture(p);
				for (const auto &input : shaderTexture.GetInputs()) {
					auto baseName = input.GetBaseName().GetString();
					auto attrTexture = input.GetAttr();
					auto y = attrTexture.GetTypeName().GetCPPTypeName();

					if (baseName == "file") {
						// Retrieve the asset file.
						pxr::SdfAssetPath assetPath;
						attrTexture.Get(&assetPath, 0);

					//	pxr::ArResolverContextBinder resolverContextBinder(p.GetStage()->GetPathResolverContext());
						pxr::ArResolver &resolver = pxr::ArGetResolver();
						resolver.RefreshContext(p.GetStage()->GetPathResolverContext());

						// FIXME: Arbitrarily replace <UDIM> with 1001. Currently unsure how to resolve this.
						if (assetPath.GetResolvedPath() == "") {
							std::string assetPathToCheck = assetPath.GetAssetPath();
							hg::replace_all(assetPathToCheck, "<UDIM>", "1001");
							auto resolvedPath = resolver.Resolve(assetPathToCheck);
							assetPath = pxr::SdfAssetPath(assetPath.GetAssetPath(), resolvedPath);
						}

						if (assetPath.GetResolvedPath() != "") {
							// Retrieve the texture.
							auto textureAsset = resolver.OpenAsset(pxr::ArResolvedPath(assetPath.GetResolvedPath()));
							std::string dst_path;
							auto findOutputPath = GetOutputPath(dst_path, config.base_output_path + "/Textures", hg::GetFileName(assetPath.GetAssetPath()), {},
								hg::GetFileExtension(assetPath.GetAssetPath()), config.import_policy_texture);

							// Retrieve the SHA1 hash of this texture and check if we already have it.
							auto sha1Picture = hg::ComputeSHA1String(textureAsset->GetBuffer().get(), textureAsset->GetSize());
							// If the SHA1 hash is not found, import the texture.
							if (picture_sha1_to_dest_path.find(sha1Picture) == picture_sha1_to_dest_path.end()) {
								picture_sha1_to_dest_path[sha1Picture] = dst_path;

								if (findOutputPath) {
									auto myfile = std::fstream(dst_path, std::ios::out | std::ios::binary);
									myfile.write(textureAsset->GetBuffer().get(), textureAsset->GetSize());
									myfile.close();
								}

								// Add ".meta" to ignore this texture from assetc (if it is used by a material, it will be overwritten).
								std::string dst_path_meta;
								if (GetOutputPath(dst_path_meta, config.base_output_path + "/Textures", hg::CutFilePath(assetPath.GetAssetPath()), {}, "meta", config.import_policy_texture)) {
									if (std::FILE *f = std::fopen(dst_path_meta.c_str(), "w")) {
										static const std::string meta_ignore_texture("{\"profiles\": {\"default\": {\"type\": \"Ignore\"}}}");
										std::fwrite(meta_ignore_texture.data(), sizeof meta_ignore_texture[0], meta_ignore_texture.size(), f);
										std::fclose(f);
									}
								}

								// Keep the saved texture.
								uint32_t flags = BGFX_SAMPLER_NONE;
								std::string dst_rel_path = MakeRelativeResourceName(dst_path, config.prj_path, config.prefix);
								auto text_ref = resources.textures.Add(dst_rel_path.c_str(), { flags, BGFX_INVALID_HANDLE });

								// Cache the texture path to the texture reference.
								picture_dest_path_to_tex_ref[dst_path] = text_ref;
							}
							else {
								// Retrieve the texture reference from the cached SHA1 and report it to the cache texture reference.
								picture_dest_path_to_tex_ref[dst_path] = picture_dest_path_to_tex_ref[picture_sha1_to_dest_path[sha1Picture]];
							}
						} else
							hg::error(hg::format("Can't find asset with path %1").arg(assetPath.GetAssetPath()));
					}
				}
			}
		}
	}

	// Export nodes.
	auto children = stage->GetPseudoRoot().GetChildren();
	for (auto p : children) {
		ExportNode(p, nullptr, scene, config, resources);
	}

	// Add default PBR map.
	scene.environment.brdf_map = resources.textures.Add("core/pbr/brdf.dds", {BGFX_SAMPLER_NONE, BGFX_INVALID_HANDLE});
	scene.environment.probe.irradiance_map = resources.textures.Add("core/pbr/probe.hdr.irradiance", {BGFX_SAMPLER_NONE, BGFX_INVALID_HANDLE});
	scene.environment.probe.radiance_map = resources.textures.Add("core/pbr/probe.hdr.radiance", {BGFX_SAMPLER_NONE, BGFX_INVALID_HANDLE});

	std::string out_path;
	if (GetOutputPath(out_path, config.base_output_path,
			config.name.empty() ? hg::GetFileName(path) : config.name, {}, "scn",
			config.import_policy_scene))
		SaveSceneJsonToFile(out_path.c_str(), scene, resources);

	hg::log(hg::format("Import complete, took %1 ms").arg(hg::time_to_ms(hg::time_now() - t_start)));
	return true;
}

static ImportPolicy ImportPolicyFromString(const std::string &v) {
	if (v == "skip")
		return ImportPolicy::SkipExisting;
	if (v == "overwrite")
		return ImportPolicy::Overwrite;
	if (v == "rename")
		return ImportPolicy::Rename;
	if (v == "skip_always")
		return ImportPolicy::SkipAlways;

	return ImportPolicy::SkipExisting;
}

static void OutputUsage(const hg::CmdLineFormat &cmd_format) {
	hg::debug((std::string("Usage: usd_importer ") + hg::word_wrap(hg::FormatCmdLineArgs(cmd_format), 80, 21) + "\n").c_str());
	hg::debug((hg::FormatCmdLineArgsDescription(cmd_format)).c_str());
}

//
static std::mutex log_mutex;
static bool quiet = false;

int main(int argc, const char **argv) {
	hg::set_log_hook(
		[](const char *msg, int mask, const char *details, void *user) {
			if (quiet && !(mask & hg::LL_Error))
				return; // skip masked entries

			std::lock_guard<std::mutex> guard(log_mutex);
			std::cout << msg << std::endl;
		},
		nullptr);
	hg::set_log_level(hg::LL_All);

	hg::debug(hg::format("USD->HG Converter %1 (%2)").arg(hg::get_version_string()).arg(hg::get_build_sha()).c_str());

	hg::CmdLineFormat cmd_format = {
		{
			{"-recalculate-normal", "Recreate the vertex normals of exported geometries"},
			{"-recalculate-tangent", "Recreate the vertex tangent frames of exported geometries"},
			{"-detect-geometry-instances", "Detect and optimize geometry instances"},
			{"-anim-to-file", "Scene animations will be exported to separate files and not embedded in scene"},
			{"-quiet", "Quiet log, only log errors"},
		},
		{
			{"-out", "Output directory", true},
			{"-base-resource-path", "Transform references to assets in this directory to be relative", true},
			{"-name", "Specify the output scene name", true},
			{"-prefix", "Specify the file system prefix from which relative assets are to be loaded from", true},
			{"-all-policy", "All file output policy (skip, overwrite, rename or skip_always) [default=skip]", true},
			{"-geometry-policy", "Geometry file output policy (skip, overwrite, rename or skip_always) [default=skip]", true},
			{"-material-policy", "Material file output policy (skip, overwrite, rename or skip_always) [default=skip]", true},
			{"-texture-policy", "Texture file output policy (skip, overwrite, rename or skip_always) [default=skip]", true},
			{"-scene-policy", "Scene file output policy (skip, overwrite, rename or skip_always) [default=skip]", true},
			{"-anim-policy",
				"Animation file output policy (skip, overwrite, rename or skip_always) (note: only applies when saving animations to their own "
				"file) [default=skip]",
				true},
			{"-geometry-scale", "Factor used to scale exported geometries", true},
			{"-finalizer-script", "Path to the Lua finalizer script", true},
			{"-shader", "Material pipeline shader [default=core/shader/pbr.hps]", true},
		},
		{
			{"input", "Input FBX file to convert"},
		},
		{
			{"-o", "-out"},
			{"-h", "-help"},
			{"-q", "-quiet"},
			{"-s", "-shader"},
		},
	};

	hg::CmdLineContent cmd_content;
	if (!hg::ParseCmdLine({argv + 1, argv + argc}, cmd_format, cmd_content)) {
		OutputUsage(cmd_format);
		return -1;
	}

	//
	Config config;
	config.base_output_path = hg::CleanPath(hg::GetCmdLineSingleValue(cmd_content, "-out", "./"));
	config.prj_path = hg::CleanPath(hg::GetCmdLineSingleValue(cmd_content, "-base-resource-path", ""));
	config.name = hg::CleanPath(hg::GetCmdLineSingleValue(cmd_content, "-name", ""));
	config.prefix = hg::GetCmdLineSingleValue(cmd_content, "-prefix", "");

	config.import_policy_anim = config.import_policy_geometry = config.import_policy_material = config.import_policy_scene = config.import_policy_texture =
		ImportPolicyFromString(hg::GetCmdLineSingleValue(cmd_content, "-all-policy", "skip"));
	config.import_policy_geometry = ImportPolicyFromString(hg::GetCmdLineSingleValue(cmd_content, "-geometry-policy", "skip"));
	config.import_policy_material = ImportPolicyFromString(hg::GetCmdLineSingleValue(cmd_content, "-material-policy", "skip"));
	config.import_policy_texture = ImportPolicyFromString(hg::GetCmdLineSingleValue(cmd_content, "-texture-policy", "skip"));
	config.import_policy_scene = ImportPolicyFromString(hg::GetCmdLineSingleValue(cmd_content, "-scene-policy", "skip"));
	config.import_policy_anim = ImportPolicyFromString(hg::GetCmdLineSingleValue(cmd_content, "-anim-policy", "skip"));

	config.geometry_scale = hg::GetCmdLineSingleValue(cmd_content, "-geometry-scale", 1.f);

	config.recalculate_normal = hg::GetCmdLineFlagValue(cmd_content, "-recalculate-normal");
	config.recalculate_tangent = hg::GetCmdLineFlagValue(cmd_content, "-recalculate-tangent");

	config.finalizer_script = hg::GetCmdLineSingleValue(cmd_content, "-finalizer-script", "");

	config.shader = hg::GetCmdLineSingleValue(cmd_content, "-shader", "");

	quiet = hg::GetCmdLineFlagValue(cmd_content, "-quiet");

	//
	if (cmd_content.positionals.size() != 1) {
		hg::debug("No input file");
		OutputUsage(cmd_format);
		return -2;
	}

	//
	config.input_path = cmd_content.positionals[0];
	auto res = ImportUSDScene(cmd_content.positionals[0], config);

	const auto msg = std::string("[ImportScene") + std::string(res ? ": OK]" : ": KO]");
	hg::log(msg.c_str());

	return res ? EXIT_SUCCESS : EXIT_FAILURE;
}
