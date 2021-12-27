#include <CadR/Geometry.h>
#include <CadR/Pipeline.h>
#include <CadR/PrimitiveSet.h>
#include <CadR/StateSet.h>
#include <CadR/VulkanDevice.h>
#include <CadR/VulkanInstance.h>
#include <CadR/VulkanLibrary.h>
#include "Window.h"
#include <vulkan/vulkan.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#if _WIN32 // MSVC 2017 and 2019
#include <filesystem>
#else // gcc 7.4.0 (Ubuntu 18.04) does support path only as experimental
#include <experimental/filesystem>
namespace std { namespace filesystem { using std::experimental::filesystem::path; } }
#endif
#include <fstream>
#include <iostream>
#include <memory>

using namespace std;

using json=nlohmann::json;

typedef logic_error gltfError;

// shader code in SPIR-V binary
static const uint32_t coordinateShaderSpirv[]={
#include "shaders/coordinates.vert.spv"
};
static const uint32_t unspecifiedMaterialShaderSpirv[]={
#include "shaders/unspecifiedMaterial.frag.spv"
};


template<typename T,typename Map,typename Key>
T mapGetWithDefault(const Map& m,const Key& k,T d)
{
	auto it=m.find(k);
	if(it!=m.end())
		return *it;
	else
		return d;
}


uint32_t getStride(int componentType,const string& type)
{
	uint32_t componentSize;
	switch(componentType) {
	case 5120:                          // BYTE
	case 5121: componentSize=1; break;  // UNSIGNED_BYTE
	case 5122:                          // SHORT
	case 5123: componentSize=2; break;  // UNSIGNED_SHORT
	case 5125:                          // UNSIGNED_INT
	case 5126: componentSize=4; break;  // FLOAT
	default: throw gltfError("Invalid accessor's componentType value.");
	}
	if(type=="VEC3")  return 3*componentSize;
	else if(type=="VEC4")  return 4*componentSize;
	else if(type=="VEC2")  return 2*componentSize;
	else if(type=="SCALAR")  return componentSize;
	else if(type=="MAT4")  return 16*componentSize;
	else if(type=="MAT3")  return 9*componentSize;
	else if(type=="MAT2")  return 4*componentSize;
	else throw gltfError("Invalid accessor's type.");
}


vk::Format getFormat(int componentType,const string& type,bool normalize,bool wantInt=false)
{
	if(componentType>=5127)
		throw gltfError("Invalid accessor's componentType.");

	// FLOAT component type
	if(componentType==5126) {
		if(normalize)
			throw gltfError("Normalize set while accessor's componentType is FLOAT (5126).");
		if(wantInt)
			throw gltfError("Integer format asked while accessor's componentType is FLOAT (5126).");
		if(type=="VEC3")       return vk::Format::eR32G32B32Sfloat;
		else if(type=="VEC4")  return vk::Format::eR32G32B32A32Sfloat;
		else if(type=="VEC2")  return vk::Format::eR32G32Sfloat;
		else if(type=="SCALAR")  return vk::Format::eR32Sfloat;
		else if(type=="MAT4")  return vk::Format::eR32G32B32A32Sfloat;
		else if(type=="MAT3")  return vk::Format::eR32G32B32Sfloat;
		else if(type=="MAT2")  return vk::Format::eR32G32Sfloat;
		else throw gltfError("Invalid accessor's type.");
	}

	// UNSIGNED_INT component type
	else if(componentType==5125)
		throw gltfError("UNSIGNED_INT componentType shall be used only for accessors containing indices. No attribute format is supported.");

	// INT component type
	else if(componentType==5124)
		throw gltfError("Invalid componentType. INT is not valid value for glTF 2.0.");

	// SHORT and UNSIGNED_SHORT component type
	else if(componentType>=5122) {
		int base;
		if(type=="VEC3")       base=84;  // VK_FORMAT_R16G16B16_UNORM
		else if(type=="VEC4")  base=91;  // VK_FORMAT_R16G16B16A16_UNORM
		else if(type=="VEC2")  base=77;  // VK_FORMAT_R16G16_UNORM
		else if(type=="SCALAR")  base=70;  // VK_FORMAT_R16_UNORM
		else if(type=="MAT4")  base=91;
		else if(type=="MAT3")  base=84;
		else if(type=="MAT2")  base=77;
		else throw gltfError("Invalid accessor's type.");
		if(componentType==5122)  // signed SHORT
			base+=1;  // VK_FORMAT_R16*_S*
		if(wantInt)    return vk::Format(base+4);  // VK_FORMAT_R16*_[U|S]INT
		if(normalize)  return vk::Format(base);    // VK_FORMAT_R16*_[U|S]NORM
		else           return vk::Format(base+2);  // VK_FORMAT_R16*_[U|S]SCALED
	}

	// BYTE and UNSIGNED_BYTE component type
	else if(componentType>=5120) {
	int base;
		if(type=="VEC3")       base=23;  // VK_FORMAT_R8G8B8_UNORM
		else if(type=="VEC4")  base=37;  // VK_FORMAT_R8G8B8A8_UNORM
		else if(type=="VEC2")  base=16;  // VK_FORMAT_R8G8_UNORM
		else if(type=="SCALAR")  base=9;  // VK_FORMAT_R8_UNORM
		else if(type=="MAT4")  base=37;
		else if(type=="MAT3")  base=23;
		else if(type=="MAT2")  base=16;
		else throw gltfError("Invalid accessor's type.");
		if(componentType==5120)  // signed BYTE
			base+=1;  // VK_FORMAT_R8*_S*
		if(wantInt)    return vk::Format(base+4);  // VK_FORMAT_R16*_[U|S]INT
		if(normalize)  return vk::Format(base);    // VK_FORMAT_R16*_[U|S]NORM
		else           return vk::Format(base+2);  // VK_FORMAT_R16*_[U|S]SCALED
	}

	// componentType bellow 5120
	throw gltfError("Invalid accessor's componentType.");
	return vk::Format(0);
}


int main(int argc,char** argv) {

	try {

		// get file to load
		if(argc<2) {
			cout<<"Please, specify glTF file to load."<<endl;
			return 1;
		}
		filesystem::path filePath(argv[1]);

		// open file
		ifstream f(filePath);
		if(!f.is_open()) {
			cout<<"Can not open file "<<filePath<<"."<<endl;
			return 1;
		}
		f.exceptions(ifstream::badbit|ifstream::failbit);

		// init Vulkan and open window
		CadR::VulkanLibrary vulkanLib(CadR::VulkanLibrary::defaultName());
		CadR::VulkanInstance vulkanInstance(vulkanLib,"glTF reader",0,"CADR",0,VK_API_VERSION_1_0,nullptr,
#ifdef _WIN32
		                                    {"VK_KHR_surface","VK_KHR_win32_surface"});  // enabled extension names
#else
		                                    {"VK_KHR_surface","VK_KHR_xlib_surface"});  // enabled extension names
#endif
		CadUI::Window window(vulkanInstance);
		tuple<vk::PhysicalDevice,uint32_t,uint32_t> deviceAndQueueFamilies=
				vulkanInstance.chooseDeviceAndQueueFamilies(window.surface());
		CadR::VulkanDevice device(vulkanInstance,deviceAndQueueFamilies,
		                          nullptr,"VK_KHR_swapchain",&vk::PhysicalDeviceFeatures().setMultiDrawIndirect(true));
		vk::PhysicalDevice physicalDevice=std::get<0>(deviceAndQueueFamilies);
		uint32_t graphicsQueueFamily=std::get<1>(deviceAndQueueFamilies);
		uint32_t presentationQueueFamily=std::get<2>(deviceAndQueueFamilies);
		CadR::Renderer renderer(device,vulkanInstance,physicalDevice,graphicsQueueFamily);
		CadR::Renderer::set(&renderer);

		// get queues
		vk::Queue graphicsQueue=device.getQueue(graphicsQueueFamily,0);
		vk::Queue presentationQueue=device.getQueue(presentationQueueFamily,0);

		// choose surface formats
		vk::SurfaceFormatKHR surfaceFormat;
		{
			vector<vk::SurfaceFormatKHR> sfList=physicalDevice.getSurfaceFormatsKHR(window.surface(),vulkanInstance);
			const vk::SurfaceFormatKHR wantedSurfaceFormat{vk::Format::eB8G8R8A8Unorm,vk::ColorSpaceKHR::eSrgbNonlinear};
			surfaceFormat=
				sfList.size()==1&&sfList[0].format==vk::Format::eUndefined
					?wantedSurfaceFormat
					:std::find(sfList.begin(),sfList.end(),
					           wantedSurfaceFormat)!=sfList.end()
						           ?wantedSurfaceFormat
						           :sfList[0];
		}
		/*vk::Format depthFormat=[](vk::PhysicalDevice physicalDevice,CadR::VulkanInstance& vulkanInstance){
			for(vk::Format f:array<vk::Format,3>{vk::Format::eD32Sfloat,vk::Format::eD32SfloatS8Uint,vk::Format::eD24UnormS8Uint}) {
				vk::FormatProperties p=physicalDevice.getFormatProperties(f,vulkanInstance);
				if(p.optimalTilingFeatures&vk::FormatFeatureFlagBits::eDepthStencilAttachment) {
					return f;
				}
			}
			throw std::runtime_error("No suitable depth buffer format.");
		}(physicalDevice,vulkanInstance);*/

		// choose presentation mode
		vk::PresentModeKHR presentMode=
			[](vector<vk::PresentModeKHR>&& modes){  // presentMode
					return find(modes.begin(),modes.end(),vk::PresentModeKHR::eMailbox)!=modes.end()
						?vk::PresentModeKHR::eMailbox
						:vk::PresentModeKHR::eFifo; // fifo is guaranteed to be supported
				}(physicalDevice.getSurfacePresentModesKHR(window.surface(),window));

		// render pass
		auto renderPass=
			device.createRenderPassUnique(
				vk::RenderPassCreateInfo(
					vk::RenderPassCreateFlags(),  // flags
					1,                            // attachmentCount
					array<const vk::AttachmentDescription,1>{  // pAttachments
						vk::AttachmentDescription{  // color attachment
							vk::AttachmentDescriptionFlags(),  // flags
							surfaceFormat.format,              // format
							vk::SampleCountFlagBits::e1,       // samples
							vk::AttachmentLoadOp::eClear,      // loadOp
							vk::AttachmentStoreOp::eStore,     // storeOp
							vk::AttachmentLoadOp::eDontCare,   // stencilLoadOp
							vk::AttachmentStoreOp::eDontCare,  // stencilStoreOp
							vk::ImageLayout::eUndefined,       // initialLayout
							vk::ImageLayout::ePresentSrcKHR    // finalLayout
						}/*,
						vk::AttachmentDescription{  // depth attachment
							vk::AttachmentDescriptionFlags(),  // flags
							depthFormat,                       // format
							vk::SampleCountFlagBits::e1,       // samples
							vk::AttachmentLoadOp::eClear,      // loadOp
							vk::AttachmentStoreOp::eDontCare,  // storeOp
							vk::AttachmentLoadOp::eDontCare,   // stencilLoadOp
							vk::AttachmentStoreOp::eDontCare,  // stencilStoreOp
							vk::ImageLayout::eUndefined,       // initialLayout
							vk::ImageLayout::eDepthStencilAttachmentOptimal  // finalLayout
						}*/
					}.data(),
					1,  // subpassCount
					&(const vk::SubpassDescription&)vk::SubpassDescription(  // pSubpasses
						vk::SubpassDescriptionFlags(),     // flags
						vk::PipelineBindPoint::eGraphics,  // pipelineBindPoint
						0,        // inputAttachmentCount
						nullptr,  // pInputAttachments
						1,        // colorAttachmentCount
						&(const vk::AttachmentReference&)vk::AttachmentReference(  // pColorAttachments
							0,  // attachment
							vk::ImageLayout::eColorAttachmentOptimal  // layout
						),
						nullptr,  // pResolveAttachments
						/*&(const vk::AttachmentReference&)vk::AttachmentReference(  // pDepthStencilAttachment
							1,  // attachment
							vk::ImageLayout::eDepthStencilAttachmentOptimal  // layout
						)*/nullptr,
						0,        // preserveAttachmentCount
						nullptr   // pPreserveAttachments
					),
					1,  // dependencyCount
					&(const vk::SubpassDependency&)vk::SubpassDependency(  // pDependencies
						VK_SUBPASS_EXTERNAL,   // srcSubpass
						0,                     // dstSubpass
						vk::PipelineStageFlags(vk::PipelineStageFlagBits::eColorAttachmentOutput),  // srcStageMask
						vk::PipelineStageFlags(vk::PipelineStageFlagBits::eColorAttachmentOutput),  // dstStageMask
						vk::AccessFlags(),     // srcAccessMask
						vk::AccessFlags(vk::AccessFlagBits::eColorAttachmentRead|vk::AccessFlagBits::eColorAttachmentWrite),  // dstAccessMask
						vk::DependencyFlags()  // dependencyFlags
					)
				)
			);

		// setup window
		window.initVulkan(physicalDevice,device,surfaceFormat,graphicsQueueFamily,
		                  presentationQueueFamily,renderPass.get(),presentMode);

		// parse json
		cout<<"Processing file "<<filePath<<"..."<<endl;
		json j3;
		f>>j3;

		// print glTF info
		cout<<endl;
		cout<<"glTF info:"<<endl;
		cout<<"   Version:     "<<j3["asset"]["version"]<<endl;
		cout<<"   MinVersion:  "<<j3["asset"]["minVersion"]<<endl;
		cout<<"   Generator:   "<<j3["asset"]["generator"]<<endl;
		cout<<"   Copyright:   "<<j3["asset"]["copyright"]<<endl;
		cout<<endl;

		// read object lists
		auto scenes=j3["scenes"];
		auto nodes=j3["nodes"];
		auto meshes=j3["meshes"];
		auto accessors=j3["accessors"];
		auto buffers=j3["buffers"];
		auto bufferViews=j3["bufferViews"];

		// print stats
		cout<<"Stats:"<<endl;
		cout<<"   Scenes:  "<<scenes.size()<<endl;
		cout<<"   Nodes:   "<<nodes.size()<<endl;
		cout<<"   Meshes:  "<<meshes.size()<<endl;
		cout<<endl;

		// pipelines
		struct PipelineAndStateSet {
			CadR::Pipeline pipeline;
			CadR::StateSet stateSet;
			PipelineAndStateSet(CadR::Renderer* r) : pipeline(r), stateSet(r)  {}
			~PipelineAndStateSet()  { pipeline.destroyPipeline(); }
		};
		array<map<CadR::AttribSizeList,PipelineAndStateSet>,10> pipelineDB;

		// CadR scene data
		vector<CadR::Geometry> geometryDB;
		vector<CadR::Drawable> drawableDB;
		CadR::StateSet stateSetRoot(&renderer);

		// shaders
		auto coordinateShader=
			device.createShaderModuleUnique(
				vk::ShaderModuleCreateInfo(
					vk::ShaderModuleCreateFlags(),  // flags
					sizeof(coordinateShaderSpirv),  // codeSize
					coordinateShaderSpirv  // pCode
				)
			);
		auto unknownMaterialShader=
			device.createShaderModuleUnique(
				vk::ShaderModuleCreateInfo(
					vk::ShaderModuleCreateFlags(),  // flags
					sizeof(unspecifiedMaterialShaderSpirv),  // codeSize
					unspecifiedMaterialShaderSpirv  // pCode
				)
			);

		// pipeline layout
		auto pipelineLayout=
			device.createPipelineLayoutUnique(
				vk::PipelineLayoutCreateInfo{
					vk::PipelineLayoutCreateFlags(),  // flags
					0,       // setLayoutCount
					nullptr, // pSetLayouts
					1,       // pushConstantRangeCount
					&(const vk::PushConstantRange&)vk::PushConstantRange{  // pPushConstantRanges
						vk::ShaderStageFlagBits::eVertex,  // stageFlags
						0,  // offset
						2*16*sizeof(float)  // size
					}
				}
			);

		// mesh
		if(meshes.size()!=1)
			throw gltfError("Only files with one mesh are supported for now.");
		auto& mesh=meshes[0];

		// primitive
		auto primitives=mesh["primitives"];
		for(auto primitive : primitives) {

			// positionAccessorIndex
			auto attributes=primitive["attributes"];
			int positionAccessorIndex=mapGetWithDefault(attributes,"POSITION",-1);

			// skip primitives without POSITION attribute
			if(positionAccessorIndex==-1)
				continue;

			// numVertices
			size_t numVertices=accessors.at(positionAccessorIndex)["count"];
			if(numVertices==0)  // skip empty primitives
				continue;

			// accessor indices
			//int normalAccessorIndex=mapGetWithDefault(attributes,"NORMAL",-1);
			//int color0AccessorIndex=mapGetWithDefault(attributes,"COLOR_0",-1);
			//int texCoord0AccessorIndex=mapGetWithDefault(attributes,"TEXCOORD_0",-1);

			// rendering mode
			auto mode=mapGetWithDefault(primitive,"mode",4);
			if(mode!=4)
				throw gltfError("Unsupported functionality: mode is not 4 (TRIANGLES).");

			// attributes
			CadR::AttribSizeList attribSizeList;
			std::vector<vk::Format> attribFormatList;
			vector<tuple<filesystem::path,size_t>> attribUriAndOffsetList;
			unsigned numProcessedAttributes=0;
			for(string name : {"POSITION","NORMAL","COLOR_0","TEXCOORD_0"}) {

				// process only known attributes
				if(attributes.find(name)==attributes.end())
					continue;
				else
					numProcessedAttributes++;

				// accessor
				auto a=accessors.at(size_t(attributes[name]));
				if(a.find("bufferView")==a.end())
					throw gltfError("Unsupported functionality: Omitted bufferView.");
				size_t bufferViewIndex=a["bufferView"];
				size_t accessorOffset=mapGetWithDefault(a,"byteOffset",0);
				int componentType=a["componentType"];
				if(componentType!=5126 && (name=="POSITION" || name=="NORMAL")) // float for POSITION and NORMAL attribute
					throw gltfError("Invalid component type. Must be float.");
				if(componentType==5125) // no UNSIGNED_INT
					throw gltfError("Invalid component type.");
				bool normalized=mapGetWithDefault(a,"normalized",false);
				if(a["count"]!=numVertices)
					throw gltfError("Attribute count does not match number of vertices inside the primitive.");
				string type=a["type"];
				if(type!="VEC3" && (name=="POSITION" || name=="NORMAL"))
					throw gltfError("Invalid attribute type. Must be VEC3.");
				if(a.find("sparse")!=a.end())
					throw gltfError("Unsupported functionality: Property sparse.");

				// bufferView
				auto bv=bufferViews.at(bufferViewIndex);
				size_t bufferIndex=bv["buffer"];
				size_t bufferViewOffset=mapGetWithDefault(bv,"byteOffset",0);
				//size_t bufferViewLength=bv["byteLength"];
				size_t stride=mapGetWithDefault(bv,"byteStride",0);
				if(stride!=0)
					throw gltfError("Unsupported functionality: Stride not zero.");

				// buffer
				auto b=buffers.at(bufferIndex);
				filesystem::path bufferUri=mapGetWithDefault<string>(b,"uri","");
				//size_t bufferLength=b["byteLength"];

				// attribSizeList
				uint32_t attribSize=getStride(componentType,type);
				attribSizeList.push_back(attribSize);

				// attribFormatList
				attribFormatList.push_back(getFormat(componentType,type,normalized,false));

				// file info
				attribUriAndOffsetList.emplace_back(
					bufferUri.is_absolute()  // file path
						?bufferUri
						:filePath.parent_path()/bufferUri,
					bufferViewOffset+accessorOffset  // file offset
				);
			}
			if(numProcessedAttributes!=attributes.size())
				throw gltfError("Unsupported attribute(s).");

			// indices
			filesystem::path indicesFileUri;
			size_t indicesFileOffset;
			size_t numIndices;
			int indicesComponentType;
			if(auto it=primitive.find("indices"); it!=primitive.end()) {
				size_t indicesAccessorIndex=*it;

				// accessor
				auto a=accessors.at(indicesAccessorIndex);
				if(a.find("bufferView")==a.end())
					throw gltfError("Unsupported functionality: Omitted bufferView for indices.");
				size_t bufferViewIndex=a["bufferView"];
				size_t accessorOffset=mapGetWithDefault(a,"byteOffset",0);
				indicesComponentType=a["componentType"];
				if(indicesComponentType!=5125 && indicesComponentType!=5123 && indicesComponentType!=5121)
					throw gltfError("Invalid component type for Indices. It must be UNSIGNED_INT, UNSIGNED_SHORT or UNSIGNED_BYTE.");
				numIndices=a["count"];
				if(numIndices==0)
					throw gltfError("Indices count in accessor is 0.");
				string type=a["type"];
				if(type!="SCALAR")
					throw gltfError("Invalid attribute type for Indices. Must be SCALAR.");
				if(a.find("sparse")!=a.end())
					throw gltfError("Unsupported functionality: Property sparse for Indices.");

				// bufferView
				auto bv=bufferViews.at(bufferViewIndex);
				size_t bufferIndex=bv["buffer"];
				size_t bufferViewOffset=mapGetWithDefault(bv,"byteOffset",0);
				//size_t bufferViewLength=bv["byteLength"];

				// buffer
				auto b=buffers.at(bufferIndex);
				filesystem::path bufferUri=mapGetWithDefault<string>(b,"uri","");
				//size_t bufferLength=b["byteLength"];

				indicesFileUri=
					bufferUri.is_absolute()
						?bufferUri
						:filePath.parent_path()/bufferUri;
				indicesFileOffset=bufferViewOffset+accessorOffset;
			}
			else {
				numIndices=numVertices;
			}
			if(numIndices>size_t(~uint32_t(0)))
				throw gltfError("Too large primitive. Index out of 32-bit integer range.");

			// create Geometry
			cout<<"Creating geometry"<<endl;
			CadR::Geometry& g=
				geometryDB.emplace_back(
					&renderer,  // renderer
					attribSizeList,  // attribSizeList
					numVertices,  // numVertices
					numIndices,  // numIndices
					1  // numPrimitiveSets
				);

			// read attributes
			unsigned i=0;
			for(auto& uriAndOffset:attribUriAndOffsetList) {

				// open buffer file
				const filesystem::path& bufferPath=std::get<0>(uriAndOffset);
				cout<<"Opening buffer "<<bufferPath<<"..."<<endl;
				ifstream f(bufferPath);
				if(!f.is_open()) {
					cout<<"Can not open file "<<bufferPath<<"."<<endl;
					return 1;
				}
				f.exceptions(ifstream::badbit|ifstream::failbit);

				// read buffer file
				CadR::StagingBuffer& sb=g.createVertexStagingBuffer(i);
				f.seekg(std::get<1>(uriAndOffset));
				f.read(reinterpret_cast<istream::char_type*>(sb.data()),sb.size());
				f.close();
				i++;
			}

			// read indices (or generate them)
			if(!indicesFileUri.empty()) {

				// open buffer file
				cout<<"Opening buffer "<<indicesFileUri<<"..."<<endl;
				ifstream f(indicesFileUri);
				if(!f.is_open()) {
					cout<<"Can not open file "<<indicesFileUri<<"."<<endl;
					return 1;
				}
				f.exceptions(ifstream::badbit|ifstream::failbit);

				// read buffer file
				CadR::StagingBuffer& sb=g.createIndexStagingBuffer();
				f.seekg(indicesFileOffset);
				if(indicesComponentType==5125)  // UNSIGNED_INT
					f.read(reinterpret_cast<istream::char_type*>(sb.data()),sb.size());
				else if(indicesComponentType==5123) {  // UNSIGNED_SHORT
					unique_ptr<uint16_t[]> tmp(new uint16_t[numIndices]);
					f.read(reinterpret_cast<istream::char_type*>(tmp.get()),numIndices*sizeof(uint16_t));
					uint32_t* b=reinterpret_cast<uint32_t*>(sb.data());
					for(size_t i=0; i<numIndices; i++)
						b[i]=tmp[i];
				}
				else if(indicesComponentType==5121) {  // UNSIGNED_BYTE
					unique_ptr<uint8_t[]> tmp(new uint8_t[numIndices]);
					f.read(reinterpret_cast<istream::char_type*>(tmp.get()),numIndices*sizeof(uint8_t));
					uint32_t* b=reinterpret_cast<uint32_t*>(sb.data());
					for(size_t i=0; i<numIndices; i++)
						b[i]=tmp[i];
				}
				f.close();
			}
			else {

				// generate indices
				if(numIndices>=size_t(~uint32_t(0)))
					throw gltfError("Too large primitive. Index out of 32-bit integer range.");
				CadR::StagingBuffer& sb=g.createIndexStagingBuffer();
				uint32_t* b=reinterpret_cast<uint32_t*>(sb.data());
				for(uint32_t i=0,e=uint32_t(numIndices); i<e; i++)
					b[i]=i;
			}

			// select pipeline
			// (main distinction here is between fixed color materials (no lighting)
			// and properly lit materials (all light computations))
			size_t pipelineIndex;
		#if 0
			if(1)  // if material base color is black
				// select fixed color pipeline
				// (selection here is made between (1) unspecified per-vertex color (white is used),
				// (2) per-vertex color, (3) emissive texture and (4) combination of emissive color and texture)
				pipelineIndex=
					(color0AccessorIndex   ==-1)?0x08:0x09;
			else
				// select lighting pipeline
				pipelineIndex=
					(texCoord0AccessorIndex==-1)?0:0x01 +
					(color0AccessorIndex   ==-1)?0:0x02 +
					(normalAccessorIndex   ==-1)?0:0x04;
		#endif
			pipelineIndex=0x08; // no COLOR_0

			auto [stateSetMapIt,newStateSetCreated] = pipelineDB[pipelineIndex].emplace(attribSizeList,&renderer);
			CadR::StateSet& ss=stateSetMapIt->second.stateSet;
			if(newStateSetCreated) {

				CadR::Pipeline* pipeline=&stateSetMapIt->second.pipeline;
				pipeline->init(nullptr,pipelineLayout.get(),nullptr);

				stateSetRoot.childList.append(ss);
				ss.pipeline=pipeline;
				ss.setGeometryStorage(g.geometryStorage());

				window.resizeCallbacks.append(
						[&device,&window,pipeline,
						&coordinateShader,&unknownMaterialShader,
						&renderer,&pipelineLayout,
						attribSizeList=std::move(attribSizeList),
						attribFormatList=std::move(attribFormatList)]()
						{

							cout<<"Resizing pipeline."<<endl;

							// construct new pipeline
							device.destroy(pipeline->get());
							pipeline->set(
								device.createGraphicsPipeline(
									renderer.pipelineCache(),
									vk::GraphicsPipelineCreateInfo(
										vk::PipelineCreateFlags(),  // flags
										2,  // stageCount
										array<const vk::PipelineShaderStageCreateInfo,2>{  // pStages
											vk::PipelineShaderStageCreateInfo{
												vk::PipelineShaderStageCreateFlags(),  // flags
												vk::ShaderStageFlagBits::eVertex,      // stage
												coordinateShader.get(),  // module
												"main",  // pName
												nullptr  // pSpecializationInfo
											},
											vk::PipelineShaderStageCreateInfo{
												vk::PipelineShaderStageCreateFlags(),  // flags
												vk::ShaderStageFlagBits::eFragment,    // stage
												unknownMaterialShader.get(),  // module
												"main",  // pName
												nullptr  // pSpecializationInfo
											}
										}.data(),
										&(const vk::PipelineVertexInputStateCreateInfo&)vk::PipelineVertexInputStateCreateInfo{  // pVertexInputState
											vk::PipelineVertexInputStateCreateFlags(),  // flags
											1,        // vertexBindingDescriptionCount
											array<const vk::VertexInputBindingDescription,1>{  // pVertexBindingDescriptions
												vk::VertexInputBindingDescription(
													0,  // binding
													attribSizeList[0],  // stride
													vk::VertexInputRate::eVertex  // inputRate
												),
											}.data(),
											1,        // vertexAttributeDescriptionCount
											array<const vk::VertexInputAttributeDescription,1>{  // pVertexAttributeDescriptions
												vk::VertexInputAttributeDescription(
													0,  // location
													0,  // binding
													attribFormatList[0],  // format
													0   // offset
												),
											}.data()
										},
										&(const vk::PipelineInputAssemblyStateCreateInfo&)vk::PipelineInputAssemblyStateCreateInfo{  // pInputAssemblyState
											vk::PipelineInputAssemblyStateCreateFlags(),  // flags
											vk::PrimitiveTopology::eTriangleList,  // topology
											VK_FALSE  // primitiveRestartEnable
										},
										nullptr, // pTessellationState
										&(const vk::PipelineViewportStateCreateInfo&)vk::PipelineViewportStateCreateInfo{  // pViewportState
											vk::PipelineViewportStateCreateFlags(),  // flags
											1,  // viewportCount
											&(const vk::Viewport&)vk::Viewport(0.f,0.f,float(window.surfaceExtent().width),float(window.surfaceExtent().height),0.f,1.f),  // pViewports
											1,  // scissorCount
											&(const vk::Rect2D&)vk::Rect2D(vk::Offset2D(0,0),window.surfaceExtent())  // pScissors
										},
										&(const vk::PipelineRasterizationStateCreateInfo&)vk::PipelineRasterizationStateCreateInfo{  // pRasterizationState
											vk::PipelineRasterizationStateCreateFlags(),  // flags
											VK_FALSE,  // depthClampEnable
											VK_FALSE,  // rasterizerDiscardEnable
											vk::PolygonMode::eFill,  // polygonMode
											vk::CullModeFlagBits::eNone,  // cullMode
											vk::FrontFace::eCounterClockwise,  // frontFace
											VK_FALSE,  // depthBiasEnable
											0.f,  // depthBiasConstantFactor
											0.f,  // depthBiasClamp
											0.f,  // depthBiasSlopeFactor
											1.f   // lineWidth
										},
										&(const vk::PipelineMultisampleStateCreateInfo&)vk::PipelineMultisampleStateCreateInfo{  // pMultisampleState
											vk::PipelineMultisampleStateCreateFlags(),  // flags
											vk::SampleCountFlagBits::e1,  // rasterizationSamples
											VK_FALSE,  // sampleShadingEnable
											0.f,       // minSampleShading
											nullptr,   // pSampleMask
											VK_FALSE,  // alphaToCoverageEnable
											VK_FALSE   // alphaToOneEnable
										},
										&(const vk::PipelineDepthStencilStateCreateInfo&)vk::PipelineDepthStencilStateCreateInfo{  // pDepthStencilState
											vk::PipelineDepthStencilStateCreateFlags(),  // flags
											VK_TRUE,  // depthTestEnable
											VK_TRUE,  // depthWriteEnable
											vk::CompareOp::eLess,  // depthCompareOp
											VK_FALSE,  // depthBoundsTestEnable
											VK_FALSE,  // stencilTestEnable
											vk::StencilOpState(),  // front
											vk::StencilOpState(),  // back
											0.f,  // minDepthBounds
											0.f   // maxDepthBounds
										},
										&(const vk::PipelineColorBlendStateCreateInfo&)vk::PipelineColorBlendStateCreateInfo{  // pColorBlendState
											vk::PipelineColorBlendStateCreateFlags(),  // flags
											VK_FALSE,  // logicOpEnable
											vk::LogicOp::eClear,  // logicOp
											1,  // attachmentCount
											&(const vk::PipelineColorBlendAttachmentState&)vk::PipelineColorBlendAttachmentState{  // pAttachments
												VK_FALSE,  // blendEnable
												vk::BlendFactor::eZero,  // srcColorBlendFactor
												vk::BlendFactor::eZero,  // dstColorBlendFactor
												vk::BlendOp::eAdd,       // colorBlendOp
												vk::BlendFactor::eZero,  // srcAlphaBlendFactor
												vk::BlendFactor::eZero,  // dstAlphaBlendFactor
												vk::BlendOp::eAdd,       // alphaBlendOp
												vk::ColorComponentFlagBits::eR|vk::ColorComponentFlagBits::eG|
													vk::ColorComponentFlagBits::eB|vk::ColorComponentFlagBits::eA  // colorWriteMask
											},
											array<float,4>{0.f,0.f,0.f,0.f}  // blendConstants
										},
										nullptr,  // pDynamicState
										pipelineLayout.get(),  // layout
										window.renderPass(),  // renderPass
										0,  // subpass
										nullptr,  // basePipelineHandle
										-1 // basePipelineIndex
									)
								)
							);

						},
						nullptr
					);
			}

			// generate one PrimitiveSet
			CadR::StagingBuffer& sb=g.createPrimitiveSetStagingBuffer();
			CadR::PrimitiveSetGpuData* b=reinterpret_cast<CadR::PrimitiveSetGpuData*>(sb.data());
			b[0]=CadR::PrimitiveSetGpuData{
					uint32_t(numIndices),  // count
					g.indexAllocation().startIndex,  // first
					g.vertexAllocation().startIndex,  // vertexOffset
					0,  // userData
				};

			// create Drawable
			drawableDB.emplace_back(g,0,0,ss);
		}

		// upload all staging buffers
		renderer.executeCopyOperations();

		// semaphores
		auto imageAvailableSemaphore=
			device.createSemaphoreUnique(
				vk::SemaphoreCreateInfo(
					vk::SemaphoreCreateFlags()  // flags
				)
			);
		auto renderingFinishedSemaphore=
			device.createSemaphoreUnique(
				vk::SemaphoreCreateInfo(
					vk::SemaphoreCreateFlags()  // flags
				)
			);

		// primaryCommandBuffers
		auto commandPool=
			device.createCommandPoolUnique(
				vk::CommandPoolCreateInfo(
					vk::CommandPoolCreateFlagBits::eTransient|vk::CommandPoolCreateFlagBits::eResetCommandBuffer,  // flags
					graphicsQueueFamily  // queueFamilyIndex
				)
			);
		vector<vk::CommandBuffer> primaryCommandBuffers;
		window.resizeCallbacks.append(
			[&device,&window,&commandPool,&primaryCommandBuffers]() {
				device.resetCommandPool(commandPool.get(),vk::CommandPoolResetFlags());
				primaryCommandBuffers=
					device.allocateCommandBuffers(
						vk::CommandBufferAllocateInfo(
							commandPool.get(),                 // commandPool
							vk::CommandBufferLevel::ePrimary,  // level
							window.imageCount()                // commandBufferCount
						)
					);
			},
			nullptr
		);


		// camera control
		float cameraHeading=0.f;
		float cameraElevation=0.f;
		float cameraDistance=5.f;
		int startMouseX,startMouseY;
		float startCameraHeading,startCameraElevation;
		window.mouseMoveCallback.append(
			[&cameraHeading,&cameraElevation,&startMouseX,&startMouseY,&startCameraHeading,&startCameraElevation]
					(int x,int y,CadUI::MouseButtons buttonState)
			{
				if(buttonState.isDown(CadUI::MouseButtons::Left)) {
					cameraHeading=startCameraHeading+(x-startMouseX)*0.01f;
					cameraElevation=startCameraElevation+(y-startMouseY)*0.01f;
				}
			}
		);
		window.mouseButtonCallback.append(
			[&cameraHeading,&cameraElevation,&startMouseX,&startMouseY,&startCameraHeading,&startCameraElevation]
					(CadUI::ButtonEventType eventType,CadUI::MouseButtons changedButton,int x,int y,CadUI::MouseButtons)
			{
				if(changedButton==CadUI::MouseButtons::Left) {
					if(eventType==CadUI::ButtonEventType::Pressed) {
						startMouseX=x;
						startMouseY=y;
						startCameraHeading=cameraHeading;
						startCameraElevation=cameraElevation;
					}
					else {
						cameraHeading=startCameraHeading+(x-startMouseX)*0.01f;
						cameraElevation=startCameraElevation+(y-startMouseY)*0.01f;
					}
				}
			}
		);
		window.mouseWheelCallback.append(
			[&cameraDistance](int z,int,int,CadUI::MouseButtons)
			{
				cameraDistance-=z;
			}
		);


		// main loop
		while(window.processEvents()) {
			if(!window.updateSize())
				continue;

			// acquire next image
			auto [imageIndex,success]=window.acquireNextImage(imageAvailableSemaphore.get());
			if(!success)
				continue;

			// record primary command buffer
			vk::CommandBuffer commandBuffer=primaryCommandBuffers[imageIndex];
			device.beginCommandBuffer(
				commandBuffer,  // commandBuffer
				vk::CommandBufferBeginInfo(
					vk::CommandBufferUsageFlagBits::eOneTimeSubmit,  // flags
					nullptr  // pInheritanceInfo
				)
			);
			glm::mat4 perspectiveMatrix(
				// make perspective:
				// ZO - Zero to One is output depth range,
				// RH - Right Hand coordinate system, +Y is down, +Z is towards camera
				// LH - LeftHand coordinate system, +Y is down, +Z points into the scene
				glm::perspectiveLH_ZO(
					glm::pi<float>()/2.f,  // fovy
					float(window.surfaceExtent().width)/window.surfaceExtent().height,  // aspect
					0.1f,  // zNear
					10.f  // zFar
				)
			);
			glm::mat4 viewMatrix(
				glm::lookAtLH(  // 0,0,+5 is produced inside translation part of viewMatrix
					glm::vec3(  // eye
						+cameraDistance*sin(-cameraHeading)*cos(cameraElevation),  // x
						-cameraDistance*sin(cameraElevation),  // y
						-cameraDistance*cos(cameraHeading)*cos(cameraElevation)  // z
					),
					glm::vec3(0.f,0.f,0.f),  // center
					glm::vec3(0.f,1.f,0.f)  // up
				)
			);
			device.cmdPushConstants(
				commandBuffer,  // commandBuffer
				pipelineLayout.get(),  // pipelineLayout
				vk::ShaderStageFlagBits::eVertex,  // stageFlags
				0,  // offset
				16*sizeof(float),  // size
				&perspectiveMatrix  // pValues
			);
			device.cmdPushConstants(
				commandBuffer,  // commandBuffer
				pipelineLayout.get(),  // pipelineLayout
				vk::ShaderStageFlagBits::eVertex,  // stageFlags
				64,  // offset
				16*sizeof(float),  // size
				&viewMatrix  // pValues
			);
			size_t numDrawables=renderer.prepareSceneRendering(stateSetRoot);
			renderer.recordDrawableProcessing(commandBuffer,numDrawables);
			renderer.recordSceneRendering(
				commandBuffer,  // commandBuffer
				stateSetRoot,  // stateSetRoot
				window.renderPass(),  // renderPass
				window.framebuffers()[imageIndex],  // framebuffer
				vk::Rect2D(vk::Offset2D(0,0),window.surfaceExtent()),  // renderArea
				1,  // clearValueCount
				&(const vk::ClearValue&)vk::ClearColorValue(array<float,4>{0.f,0.f,1.f,1.f})  // clearValues
			);
			device.endCommandBuffer(commandBuffer);

			// render
			device.queueSubmit(
				graphicsQueue,  // queue
				vk::SubmitInfo(
					1,&imageAvailableSemaphore.get(),    // waitSemaphoreCount+pWaitSemaphores
					&(const vk::PipelineStageFlags&)vk::PipelineStageFlags(vk::PipelineStageFlagBits::eColorAttachmentOutput),  // pWaitDstStageMask
					1,&commandBuffer,                    // commandBufferCount+pCommandBuffers
					1,&renderingFinishedSemaphore.get()  // signalSemaphoreCount+pSignalSemaphores
				),
				vk::Fence(nullptr)  // fence
			);
			window.present(presentationQueue,renderingFinishedSemaphore.get(),imageIndex);

			// wait for rendering to complete
			device.queueWaitIdle(presentationQueue);
		}

		// finish all pending work on device
		device.waitIdle();

	// catch exceptions
	} catch(CadR::Error &e) {
		cout<<"Failed because of CadR exception: "<<e.what()<<endl;
	} catch(vk::Error &e) {
		cout<<"Failed because of Vulkan exception: "<<e.what()<<endl;
	} catch(exception &e) {
		cout<<"Failed because of exception: "<<e.what()<<endl;
	} catch(...) {
		cout<<"Failed because of unspecified exception."<<endl;
	}

	return 0;
}
