/*
* Vulkan Example - Basic indexed triangle rendering
*
* Note:
*	This is a "pedal to the metal" example to show off how to get Vulkan up and displaying something
*	Contrary to the other examples, this one won't make use of helper functions or initializers
*	Except in a few cases (swap chain setup e.g.)
*
* Copyright (C) 2016-2017 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <fstream>
#include <vector>
#include <exception>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vulkan/vulkan.h>
#include "vulkanexamplebase.h"

// 开启验证层
#define ENABLE_VALIDATION true
// staging buffers 上传顶点和索引数据到设备的本地内存，在 prepareVertices 函数中详解
#define USE_STAGING true	

class VulkanExample : public VulkanExampleBase
{
public:
	struct Vertex {
		float position[3];
		float color[3];
	};

	struct { 
		VkDeviceMemory memory;
		VkBuffer buffer;
	} vertices;	

	struct {
		VkDeviceMemory memory;
		VkBuffer buffer;
		uint32_t count;
	} indices;

	struct {
		VkDeviceMemory memory;
		VkBuffer buffer;
		VkDescriptorBufferInfo descriptor;
	} uniformBufferVS;
	
	// 在shader中的layout布局应该保持一致，这样可以直接 memcopy 数据到GPU
	// 数据类型应该与GPU的绑定，从而避免手动填充
	struct {
		glm::mat4 projectionMatrix;
		glm::mat4 modelMatrix;
		glm::mat4 viewMatrix;
	} uboVS;

	VkPipelineLayout pipelineLayout;
	VkPipeline pipeline;
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet descriptorSet;

	VkSemaphore presentCompleteSemphore;
	VkSemaphore renderCompleteSemphore;

	std::vector<VkFence> queueCompleteFence;

	VulkanExample() : VulkanExampleBase(ENABLE_VALIDATION)
	{
		title = "";
		settings.overlay = false;

		camera.type = Camera::CameraType::lookat;
		camera.setPosition(glm::vec3(0.0f, 0.0f, -2.5f));
		camera.setRotation(glm::vec3(0.0f));
		camera.setPerspective(60.0f, (float)width / (float)height, 1.0f, 256.0f);
	}

	~VulkanExample()
	{
		vkDestroyPipeline(device, pipeline, nullptr);  
		
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);

		vkDestroyBuffer(device, vertices.buffer, nullptr);
		vkFreeMemory(device, vertices.memory, nullptr);

		vkDestroyBuffer(device, indices.buffer, nullptr);
		vkFreeMemory(device, indices.memory, nullptr);

		vkDestroyBuffer(device, uniformBufferVS.buffer, nullptr);
		vkFreeMemory(device, uniformBufferVS.memory, nullptr);

		vkDestroySemaphore(device, presentCompleteSemphore, nullptr);
		vkDestroySemaphore(device, renderCompleteSemphore, nullptr);

		for(auto fence:queueCompleteFence)
		{
			vkDestroyFence(device, fence, nullptr);
		}
	}

	/*
	* 请求一个设备内存类型，成功则返回内存类型的索引
	* @param properties 类型为uint32_t
	* @ref memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	*/
	uint32_t getMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties)
	{
		// deviceMemoryProperties: Stores all available memory (type) properties for the physical device
		for(uint32_t i=0; i < VulkanExampleBase::deviceMemoryProperties.memoryTypeCount; ++i)
		{
			if ((typeBits & 1) == 1)
			{
				if ((deviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == 1)
				{
					return i;
				}
			}
			typeBits >>= 1;
		}

		throw "Could not find a suitable memory type!";
	}

	void prepareSynchronizationPrimitives()
	{
		VkSemaphoreCreateInfo semaphoreCreateInfo = {};
		semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		semaphoreCreateInfo.pNext = nullptr;

		VK_CHECK_RESULT(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &presentCompleteSemphore));
		VK_CHECK_RESULT(vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &renderCompleteSemphore));

		VkFenceCreateInfo fenceCreateInfo;
		fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceCreateInfo.pNext = nullptr;
		fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		queueCompleteFence.resize(drawCmdBuffers.size());
		for(auto& fence : queueCompleteFence)
		{
			VK_CHECK_RESULT(vkCreateFence(device, &fenceCreateInfo, nullptr, &fence));
		}
	}

	// 如果begin==true; 可以开始向命令缓冲区中添加命令
	VkCommandBuffer getCommandBuffer(bool begin)
	{
		VkCommandBuffer cmdBuffer;
		VkCommandBufferAllocateInfo cmdBufferAI = {};
		cmdBufferAI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		cmdBufferAI.commandPool = cmdPool;
		cmdBufferAI.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		cmdBufferAI.commandBufferCount = 1;

		VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufferAI, &cmdBuffer));

		if(begin)
		{
			VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
			VK_CHECK_RESULT(vkBeginCommandBuffer(cmdBuffer, &cmdBufInfo));
		}

		return cmdBuffer;
	}

	// 关闭命令池，提交命令到队列，通过fence确保命令池在被删除前被执行完毕
	// End the command buffer and submit it to the queue
	// Uses a fence to ensure command buffer has finished executing before deleting it
	void flushCommandBuffer(VkCommandBuffer cmdBuffer)
	{
		assert(cmdBuffer != VK_NULL_HANDLE);

		VK_CHECK_RESULT(vkEndCommandBuffer(cmdBuffer));

		// 将命令池提交到queeu
		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers = &cmdBuffer;

		VkFenceCreateInfo fenceCI = {};
		fenceCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		// VkFenceCreateFlags is a bitmask type for setting a mask of zero or more VkFenceCreateFlagBits.
		// flags is a bitmask of VkFenceCreateFlagBits specifying the initial state and behavior of the fence.
		fenceCI.flags = 0;
		VkFence fence;
		VK_CHECK_RESULT(vkCreateFence(device, &fenceCI, nullptr, &fence));

		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence));
		VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));

		vkDestroyFence(device, fence, nullptr);
		vkFreeCommandBuffers(device, cmdPool, 1, &cmdBuffer);
	}

	// 为每一个framebuffer 图像创建分离的命令缓冲区
	void buildCommandBuffers()
	{
		VkCommandBufferBeginInfo cmdBufferBI = {};
		cmdBufferBI.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
		cmdBufferBI.pNext = nullptr;

		// 帧缓冲附件 loadOp 设置清零值
		VkClearValue clearValues[2];
		clearValues[0].color = {{0.0f, 0.0f, 0.2f, 1.0f}};
		clearValues[1].depthStencil = {1.0f, 0};

		VkRenderPassBeginInfo renderPassBI = {};
		renderPassBI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO;
		renderPassBI.renderPass = renderPass;
		renderPassBI.renderArea.offset.x = 0;
		renderPassBI.renderArea.offset.y = 0;
		renderPassBI.renderArea.extent.width = width;
		renderPassBI.renderArea.extent.height = height;
		renderPassBI.clearValueCount = 2;
		renderPassBI.pClearValues = clearValues;

		for (int32_t i = 0; i < drawCmdBuffers.size(); ++i)
		{
			renderPassBI.framebuffer = frameBuffers[i];
			
			VK_CHECK_RESULT(vkBeginCommandBuffer(drawCmdBuffers[i], &cmdBufferBI));
			vkCmdBeginRenderPass(drawCmdBuffers[i], &renderPassBI, VK_SUBPASS_CONTENTS_INLINE);

			VkViewport viewport = {};
			viewport.height = (float)height;
			viewport.width = (float)width;
			viewport.minDepth = (float)0.0f;
			viewport.maxDepth = (float)1.0f;
			vkCmdSetViewport(drawCmdBuffers[i], 0, 1, &viewport);

			VkRect2D scissor = {};
			scissor.extent.width = width;
			scissor.extent.height = height;
			scissor.offset.x = 0;
			scissor.offset.y = 0;
			vkCmdSetScissor(drawCmdBuffers[i], 0, 1, &scissor);

			vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
									0, 1, &descriptorSet, 0, nullptr);

			vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			
			// 绑定vertex buffer
			VkDeviceSize offsets[1] = {0};
			vkCmdBindVertexBuffers(drawCmdBuffers[i], 0, 1, &vertices.buffer, offsets);
		
			// 绑定index buffer
			vkCmdBindIndexBuffer(drawCmdBuffers[i], indices.buffer, 0, VK_INDEX_TYPE_UINT32);

			vkCmdDrawIndexed(drawCmdBuffers[i], indices.count, 1, 0, 0, 1);
			vkCmdEndRenderPass(drawCmdBuffers[i]);

			VK_CHECK_RESULT(vkEndCommandBuffer(drawCmdBuffers[i]));
		}
	}

	void draw()
	{
#if defined(VK_USE_PLATFORM_MACOS_MVK)
		prepareFrame();
		VK_CHECK_RESULT(vkWaitForFences(device, 1, &waitFences[currentBuffer], VK_TRUE, UINT64_MAX));
		VK_CHECK_RESULT(vkResetFences(device, 1, &waitFences[currentBuffer]));
#else
		VkResult acquire = swapChain.acquireNextImage(presentCompleteSemphore, &currentBuffer);
		if (!((acquire == VK_SUCCESS) || (acquire == VK_SUBOPTIMAL_KHR))) {
			VK_CHECK_RESULT(acquire);
		}
		VK_CHECK_RESULT(vkWaitForFences(device, 1, &queueCompleteFence[currentBuffer], VK_TRUE, UINT64_MAX));
		VK_CHECK_RESULT(vkResetFences(device, 1, &queueCompleteFence[currentBuffer]));
#endif
		VkPipelineStageFlags waitStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submitInfo = {};
		submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.pWaitDstStageMask = &waitStageMask;
		submitInfo.signalSemaphoreCount = 1;
		submitInfo.waitSemaphoreCount = 1;
		submitInfo.pCommandBuffers = &drawCmdBuffers[currentBuffer];
		submitInfo.commandBufferCount = 1;

#if defined(VK_USE_PLATFORM_MACOS_MVK)
		// SRS - on macOS use swapchain helper function with common semaphores/fences for proper resize handling
		submitInfo.pWaitSemaphores = &semaphores.presentComplete;    // Semaphore(s) to wait upon before the submitted command buffer starts executing
		submitInfo.pSignalSemaphores = &semaphores.renderComplete;   // Semaphore(s) to be signaled when command buffers have completed

		// Submit to the graphics queue passing a wait fence
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, waitFences[currentBuffer]));

		// Present the current buffer to the swap chain
		submitFrame();

#else
		submitInfo.pWaitSemaphores = &presentCompleteSemphore;
		submitInfo.pSignalSemaphores = &renderCompleteSemphore;

		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, queueCompleteFence[currentBuffer]));

		VkResult present = swapChain.queuePresent(queue, currentBuffer, renderCompleteSemphore);
		if(!((present == VK_SUCCESS) || (present == VK_SUBOPTIMAL_KHR))){
			VK_CHECK_RESULT(present);
		}
#endif
	}

	// 准备vertex和index 缓冲，上传到设备逻辑内存，顶点着色器
	void prepareVertices(bool useStagingBuffers)
	{
		std::vector<Vertex> vertexBuffer = 
		{
			{ {  1.0f,  1.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } },
			{ { -1.0f,  1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } },
			{ {  0.0f, -1.0f, 0.0f }, { 0.0f, 0.0f, 1.0f } }
		};
		uint32_t vertexBufferSize = static_cast<uint32_t>(vertexBuffer.size() * sizeof(Vertex));

		std::vector<uint32_t> indexBuffer = {0, 1, 2};
		indices.count = static_cast<uint32_t>(indexBuffer.size());
		uint32_t indexBufferSize = indices.count * sizeof(uint32_t);

		VkMemoryAllocateInfo memAllocIf = {};
		memAllocIf.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		VkMemoryRequirements memReqs;
		void *data;

		// CPU中的顶点数据的内存类型，不能完美适配GPU，所以需要通过在CPU上开辟staging buffers(临时缓冲)将顶点数据通过
		// buffer copy命令传送到设备本地内存
		if(useStagingBuffers)
		{
			// 静态数据如vertex和index应该存储到设备内存，被GPU接收
			// 这个过程称为：staging buffers
			// 1 创建对host可见的缓冲区
			// 2 复制data到该缓冲区
			// 3 创建设备上的另一个相同大小的本地缓冲
			// 4 把data从host复制到设备，使用命令缓冲区
			// 5 删除host的缓冲区（the host visible (staging) buffer）
			// 6 使用设备的本地缓冲来做渲染
			struct StagingBuffer{
				VkDeviceMemory memory;
				VkBuffer buffer;
			};

			struct {
				StagingBuffer vertices;
				StagingBuffer indices;
			} stagingBuffers;

			// 1 创建Vertex缓冲
			VkBufferCreateInfo vertexBufferInfo = {};
			vertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			vertexBufferInfo.size = vertexBufferSize;
			vertexBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

			VK_CHECK_RESULT(vkCreateBuffer(device, &vertexBufferInfo, nullptr, &stagingBuffers.vertices.buffer));
			vkGetBufferMemoryRequirements(device, stagingBuffers.vertices.buffer, &memReqs);
			memAllocIf.allocationSize = memReqs.size;
			memAllocIf.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocIf, nullptr, &stagingBuffers.vertices.memory));

			// 2 复制
			VK_CHECK_RESULT(vkMapMemory(device, stagingBuffers.vertices.memory, 0, memAllocIf.allocationSize, 0, &data));
			memcpy(data, vertexBuffer.data(), vertexBufferSize);
			vkUnmapMemory(device, stagingBuffers.vertices.memory);
			VK_CHECK_RESULT(vkBindBufferMemory(device, stagingBuffers.vertices.buffer, stagingBuffers.vertices.memory, 0));

			// 3 设备本地缓冲
			vertexBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			VK_CHECK_RESULT(vkCreateBuffer(device, &vertexBufferInfo, nullptr, &vertices.buffer));
			vkGetBufferMemoryRequirements(device, vertices.buffer, &memReqs);
			memAllocIf.allocationSize = memReqs.size;
			memAllocIf.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocIf, nullptr, &vertices.memory));
			VK_CHECK_RESULT(vkBindBufferMemory(device, vertices.buffer, vertices.memory, 0));

			// 对index data执行与vertex data的相同操作
			VkBufferCreateInfo indexbufferInfo = {};
			indexbufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			indexbufferInfo.size = indexBufferSize;
			indexbufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

			VK_CHECK_RESULT(vkCreateBuffer(device, &indexbufferInfo, nullptr, &stagingBuffers.indices.buffer));
			vkGetBufferMemoryRequirements(device, stagingBuffers.indices.buffer, &memReqs);
			memAllocIf.allocationSize = memReqs.size;
			memAllocIf.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocIf, nullptr, &stagingBuffers.indices.memory));
			VK_CHECK_RESULT(vkMapMemory(device, stagingBuffers.indices.memory, 0, indexBufferSize, 0, &data));
			memcpy(data, indexBuffer.data(), indexBufferSize);
			vkUnmapMemory(device, stagingBuffers.indices.memory);
			VK_CHECK_RESULT(vkBindBufferMemory(device, stagingBuffers.indices.buffer, stagingBuffers.indices.memory, 0));

			indexbufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			VK_CHECK_RESULT(vkCreateBuffer(device, &indexbufferInfo, nullptr, &indices.buffer));
			vkGetBufferMemoryRequirements(device, indices.buffer, &memReqs);
			memAllocIf.allocationSize = memReqs.size;
			memAllocIf.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocIf, nullptr, &indices.memory));
			VK_CHECK_RESULT(vkBindBufferMemory(device, indices.buffer, indices.memory, 0));

			// 4 使用command buffer将data从host复制到设备
			// Note: 有些设备提供dedicated transfer queue来加速需要大量copy的操作
			VkCommandBuffer copyCmd = getCommandBuffer(true);
			VkBufferCopy copyRegion = {};
			copyRegion.size = vertexBufferSize;
			vkCmdCopyBuffer(copyCmd, stagingBuffers.vertices.buffer, vertices.buffer, 1, &copyRegion);
			copyRegion.size = indexBufferSize;
			vkCmdCopyBuffer(copyCmd, stagingBuffers.indices.buffer, indices.buffer, 1, &copyRegion);

			// 关闭命令池，并提交命令到队列
			flushCommandBuffer(copyCmd);

			// 5 删除staging buffer
			vkDestroyBuffer(device, stagingBuffers.vertices.buffer, nullptr);
			vkFreeMemory(device, stagingBuffers.vertices.memory, nullptr);
			vkDestroyBuffer(device, stagingBuffers.indices.buffer, nullptr);
			vkFreeMemory(device, stagingBuffers.indices.memory, nullptr);

			// 6 使用设备的local buffer进行渲染
		}
		else
		{
			// 如果不使用staging buffer，那么直接使用host-visible buffers进行rendering，这样做会降低渲染性能
			VkBufferCreateInfo vertexBufferInfo = {};
			vertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			vertexBufferInfo.size = vertexBufferSize;
			vertexBufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
			VK_CHECK_RESULT(vkCreateBuffer(device, &vertexBufferInfo, nullptr, &vertices.buffer));
			vkGetBufferMemoryRequirements(device, vertices.buffer, &memReqs);
			memAllocIf.allocationSize = memReqs.size;
			memAllocIf.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocIf, nullptr, &vertices.memory));
			VK_CHECK_RESULT(vkMapMemory(device, vertices.memory, 0, memAllocIf.allocationSize, 0, &data));
			memcpy(data, vertexBuffer.data(), vertexBufferSize);
			vkUnmapMemory(device, vertices.memory);
			VK_CHECK_RESULT(vkBindBufferMemory(device, vertices.buffer, vertices.memory, 0));

			VkBufferCreateInfo indexBufferInfo = {};
			indexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
			indexBufferInfo.size = indexBufferSize;
			indexBufferInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
			VK_CHECK_RESULT(vkCreateBuffer(device, &indexBufferInfo, nullptr, &indices.buffer));
			vkGetBufferMemoryRequirements(device, indices.buffer, &memReqs);
			memAllocIf.allocationSize = memReqs.size;
			memAllocIf.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocIf, nullptr, &indices.memory));
			VK_CHECK_RESULT(vkMapMemory(device, indices.memory, 0, memAllocIf.allocationSize, 0, &data));
			memcpy(data, vertexBuffer.data(), indexBufferSize);
			vkUnmapMemory(device, indices.memory);
			VK_CHECK_RESULT(vkBindBufferMemory(device, indices.buffer, indices.memory, 0));
		}
	}

	void setupDescriptorPool()
	{
		VkDescriptorPoolSize typeCount[1];
		typeCount[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		typeCount[0].descriptorCount = 1;

		VkDescriptorPoolCreateInfo descriptorPoolCI = {};
		descriptorPoolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		descriptorPoolCI.pNext = nullptr;
		descriptorPoolCI.poolSizeCount = 1;
		descriptorPoolCI.pPoolSizes = typeCount;
		descriptorPoolCI.maxSets = 1;

		VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolCI, nullptr, &descriptorPool));
	}

	void setupDescriptorSetLayout()
	{
		VkDescriptorSetLayoutBinding layoutBinding = {};
		layoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		layoutBinding.descriptorCount = 1;
		layoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
		layoutBinding.pImmutableSamplers = nullptr;

		VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCI = {};
		descriptorSetLayoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
		descriptorSetLayoutCI.bindingCount = 1;
		descriptorSetLayoutCI.pNext = nullptr;
		descriptorSetLayoutCI.pBindings = &layoutBinding;

		VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCI, nullptr, &descriptorSetLayout));

		VkPipelineLayoutCreateInfo pipelineLayoutCI = {};
		pipelineLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
		pipelineLayoutCI.pNext = nullptr;
		pipelineLayoutCI.setLayoutCount = 1;
		pipelineLayoutCI.pSetLayouts = &descriptorSetLayout;

		VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCI, nullptr, &pipelineLayout));
	}

	void setupDescriptorSet()
	{
		VkDescriptorSetAllocateInfo descriptorSetAI = {};
		descriptorSetAI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		descriptorSetAI.descriptorSetCount = 1;
		descriptorSetAI.descriptorPool = descriptorPool;
		// descriptorSetAI.pNext = nullptr;
		descriptorSetAI.pSetLayouts = &descriptorSetLayout;

		VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &descriptorSetAI, &descriptorSet));

		VkWriteDescriptorSet writeDescriptorSet = {};
		writeDescriptorSet.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writeDescriptorSet.dstSet = descriptorSet;
		writeDescriptorSet.descriptorCount = 1;
		writeDescriptorSet.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		writeDescriptorSet.pBufferInfo = &uniformBufferVS.descriptor;
		writeDescriptorSet.dstBinding = 0;

		vkUpdateDescriptorSets(device, 1, &writeDescriptorSet, 0, nullptr);
	}

	// Create the depth (and stencil) buffer attachments used by our framebuffers
	// Note: Override of virtual function in the base class and called from within VulkanExampleBase::prepare
	void setupDepthStencil()
	{
		// Create an optimal image used as the depth stencil attachment
		VkImageCreateInfo image = {};
		image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
		image.imageType = VK_IMAGE_TYPE_2D;
		image.format = depthFormat;
		// Use example's height and width
		image.extent = { width, height, 1 };
		image.mipLevels = 1;
		image.arrayLayers = 1;
		image.samples = VK_SAMPLE_COUNT_1_BIT;
		image.tiling = VK_IMAGE_TILING_OPTIMAL;
		image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
		image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &depthStencil.image));

		// Allocate memory for the image (device local) and bind it to our image
		VkMemoryAllocateInfo memAlloc = {};
		memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		VkMemoryRequirements memReqs;
		vkGetImageMemoryRequirements(device, depthStencil.image, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		memAlloc.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &depthStencil.mem));
		VK_CHECK_RESULT(vkBindImageMemory(device, depthStencil.image, depthStencil.mem, 0));

		// Create a view for the depth stencil image
		// Images aren't directly accessed in Vulkan, but rather through views described by a subresource range
		// This allows for multiple views of one image with differing ranges (e.g. for different layers)
		VkImageViewCreateInfo depthStencilView = {};
		depthStencilView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
		depthStencilView.format = depthFormat;
		depthStencilView.subresourceRange = {};
		depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
		// Stencil aspect should only be set on depth + stencil formats (VK_FORMAT_D16_UNORM_S8_UINT..VK_FORMAT_D32_SFLOAT_S8_UINT)
		if (depthFormat >= VK_FORMAT_D16_UNORM_S8_UINT)
			depthStencilView.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;

		depthStencilView.subresourceRange.baseMipLevel = 0;
		depthStencilView.subresourceRange.levelCount = 1;
		depthStencilView.subresourceRange.baseArrayLayer = 0;
		depthStencilView.subresourceRange.layerCount = 1;
		depthStencilView.image = depthStencil.image;
		VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilView, nullptr, &depthStencil.view));
	}


	// 创建帧缓冲的深度和模板缓冲附件
	// Note：继承基类的虚函数
	void setupFrameBuffer()
	{
		// 为交换链中的每一幅图像创建一个帧缓冲
		frameBuffers.resize(swapChain.imageCount);
		for (size_t i = 0; i < frameBuffers.size(); ++i)
		{
			std::array<VkImageView, 2> attachments;
			attachments[0] = swapChain.buffers[i].view;
			attachments[1] = depthStencil.view;

			VkFramebufferCreateInfo framebufferCI = {};
			framebufferCI.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
			framebufferCI.pAttachments = attachments.data();
			framebufferCI.attachmentCount = attachments.size();
			framebufferCI.renderPass = renderPass;
			framebufferCI.width = width;
			framebufferCI.height = height;
			framebufferCI.layers = 1;
			VK_CHECK_RESULT(vkCreateFramebuffer(device, &framebufferCI, nullptr, &frameBuffers[i]));
		}
	}

	// renderPass是vulkan的新概念，定义了一组subpass，subpass包括一个或多个绘制命令并定义了attachment、subresource、图像布局转换等属性
	// 意义：明确驱动执行图形管线进行渲染的一系列步骤，帮助驱动预先分配和优化内存；驱动可以对多个subpass进行隐式的多线程并行处理。
	// 执行renderPass时，每个subPass会按照指定的顺序依次执行，将渲染结果传递给下一个subPass或最终输出
	// SubPass dependencies的作用：定义和管理子渲染之间的内存依赖关系，确保renderPass正确执行。
	void setupRenderPass()
	{
		std::array<VkAttachmentDescription, 2> attachments = {};
		// 颜色attachment
		attachments[0].format = swapChain.colorFormat;
		attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;	// renderPass结束时，颜色附件被转化为PRESENT_KGR，提交到swapchain
		// 深度attachment
		attachments[1].format = depthFormat;
		attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
		attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;	// 结束时被转换为 depth/stencil 附件

		VkAttachmentReference colorReference = {};
		colorReference.attachment = 0;
		colorReference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

		VkAttachmentReference depthReference = {};
		colorReference.attachment = 1;
		colorReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

		// subPass的描述 结构体
		VkSubpassDescription subpassDes = {};
		subpassDes.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;	// 指定使用的管线类型
		subpassDes.colorAttachmentCount = 1;
		subpassDes.pColorAttachments = &colorReference;
		subpassDes.pDepthStencilAttachment = &depthReference;	// 描述深度/模板attachment在当前subPass中的使用
		subpassDes.inputAttachmentCount = 0;	// 从上一个subPass传递过来的图像数据如何在当前subPass使用
		subpassDes.pInputAttachments = nullptr;
		subpassDes.preserveAttachmentCount = 0;	// 保留上一个subPass未修改的图像数据
		subpassDes.pPreserveAttachments = nullptr;
		subpassDes.pResolveAttachments = nullptr;	// 指定每个color attachment对应的解析附着物，用于多采样渲染的解析操作

		// 定义子渲染流程之间的依赖关系，描述子渲染流程将等待哪些条件完成后再开始执行，以及执行结束后触发哪些操作
		std::array<VkSubpassDependency, 2> dependencies = {};
		// 深度attachment
		dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[0].dstSubpass = 0;
		dependencies[0].srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
		dependencies[0].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
		dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
		dependencies[0].dependencyFlags = 0;
		// 颜色attachment
		dependencies[1].srcSubpass = VK_SUBPASS_EXTERNAL;
		dependencies[1].dstSubpass = 0;
		dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependencies[1].srcAccessMask = 0;
		dependencies[1].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
		dependencies[1].dependencyFlags = 0;

		VkRenderPassCreateInfo renderPassCI = {};
		renderPassCI.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
		renderPassCI.attachmentCount = static_cast<uint32_t>(attachments.size());
		renderPassCI.pAttachments = attachments.data();
		renderPassCI.subpassCount = 1;
		renderPassCI.pSubpasses = &subpassDes;
		renderPassCI.dependencyCount = static_cast<uint32_t>(dependencies.size());
		renderPassCI.pDependencies = dependencies.data();

		VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassCI, nullptr, &renderPass));
	}

	// SPIR-V是一种与硬件和API无关的着色器语言，通过开源SRIPV-Tools库或Glslang工具编译生成
	VkShaderModule loadSRIPVShader(std::string filename)
	{
		size_t shaderSize;
		char *shaderCode = NULL;

#if defined(__ANDROID__)
		// Load shader from compressed asset
		AAsset* asset = AAssetManager_open(androidApp->activity->assetManager, filename.c_str(), AASSET_MODE_STREAMING);
		assert(asset);
		shaderSize = AAsset_getLength(asset);
		assert(shaderSize > 0);

		shaderCode = new char[shaderSize];
		AAsset_read(asset, shaderCode, shaderSize);
		AAsset_close(asset);
#else
		std::ifstream is(filename, std::ios::binary | std::ios::in | std::ios::ate);

		if (is.is_open())
		{
			shaderSize = is.tellg();
			is.seekg(0, std::ios::beg);
			// Copy file contents into a buffer
			shaderCode = new char[shaderSize];
			is.read(shaderCode, shaderSize);
			is.close();
			assert(shaderSize > 0);
		}
#endif
		if(shaderCode)
		{
			VkShaderModuleCreateInfo shaderModuleCI = {};
			shaderModuleCI.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			shaderModuleCI.codeSize = shaderSize;
			shaderModuleCI.pCode = reinterpret_cast<uint32_t*>(shaderCode);

			VkShaderModule shaderModule;
			VK_CHECK_RESULT(vkCreateShaderModule(device, &shaderModuleCI, NULL, &shaderModule));

			delete[] shaderCode;

			return shaderModule;
		}
		else
		{
			std::cerr << "Error: Could not open shader file\" " << filename << "\"" << std::endl;
			return VK_NULL_HANDLE;
		}
	}

	void preparePipeline()
	{
		VkGraphicsPipelineCreateInfo pipelineCI = {};
		pipelineCI.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
		pipelineCI.layout = pipelineLayout;
		pipelineCI.renderPass = renderPass;

		// 图元装配
		VkPipelineInputAssemblyStateCreateInfo inputAssemblyStateCI = {};
		inputAssemblyStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
		inputAssemblyStateCI.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;	// 图元拓扑类型为三角形

		// 光栅化状态
		VkPipelineRasterizationStateCreateInfo rasterizationStateCI = {};
		rasterizationStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
		rasterizationStateCI.polygonMode = VK_POLYGON_MODE_FILL;	// 指定光栅化模式，如点POINT、线LINE、填充模式FILL
		rasterizationStateCI.cullMode = VK_CULL_MODE_NONE;	// 指定剔除模式，如不剔除NONE、正面剔除FRONT_BIT、背面剔除BACK_BIT
		rasterizationStateCI.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;	// 指定前向方向，如顺时针CLOCKWISE和逆时针COUNTER_CLOCKWISE
		rasterizationStateCI.depthClampEnable = VK_FALSE;	// 是否启用深度裁剪
		rasterizationStateCI.rasterizerDiscardEnable = VK_FALSE;	// 是否启用光栅化丢弃，为true则不执行光栅化操作
		rasterizationStateCI.depthBiasEnable = VK_FALSE;	// 是否启用深度偏移，启用来解决z-fighting问题
		rasterizationStateCI.lineWidth = 1.0f;	// 指定线段的宽度，只在线模式下有效

		// 颜色混合模式
		VkPipelineColorBlendAttachmentState blendAttachmentState[1] = {};
		blendAttachmentState[0].colorWriteMask = 0xf;
		blendAttachmentState[0].blendEnable = VK_FALSE;
		VkPipelineColorBlendStateCreateInfo colorBlendStateCI = {};
		colorBlendStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
		colorBlendStateCI.attachmentCount = 1;
		colorBlendStateCI.pAttachments = blendAttachmentState;

		VkPipelineViewportStateCreateInfo viewportStateCI = {};
		viewportStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
		viewportStateCI.viewportCount = 1;
		viewportStateCI.scissorCount = 1;

		// 视口和裁剪矩阵是创建管线时静态指定，而线宽、深度偏移、混合参数等可以在管线运行时动态修改
		std::vector<VkDynamicState> dynamicStateEnables;
		dynamicStateEnables.push_back(VK_DYNAMIC_STATE_VIEWPORT);	// 在管线运行时，使用vkCmdSetViewport等动态修改状态
		dynamicStateEnables.push_back(VK_DYNAMIC_STATE_SCISSOR);	
		VkPipelineDynamicStateCreateInfo dynamicStateCI = {};
		dynamicStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
		dynamicStateCI.pDynamicStates = dynamicStateEnables.data();
		dynamicStateCI.dynamicStateCount = static_cast<uint32_t>(dynamicStateEnables.size());

		// 设置深度和模板测试状态
		VkPipelineDepthStencilStateCreateInfo depthStencilStateCI = {};
		depthStencilStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
		depthStencilStateCI.depthTestEnable = VK_TRUE;	// 是否启用深度测试
		depthStencilStateCI.depthWriteEnable = VK_TRUE;	// 启用深度写入
		depthStencilStateCI.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;	// 深度测试的比较操作方式
		depthStencilStateCI.depthBoundsTestEnable = VK_FALSE;	// 深度边界测试
		// 如果启用则在minDepthBounds和maxDepthBounds之间的片段被保留，其余的被剔除
		// depthStencilStateCI.minDepthBounds = 0.0f;
		// depthStencilStateCI.maxDepthBounds = 1.0f;
		depthStencilStateCI.back.failOp = VK_STENCIL_OP_KEEP;
		depthStencilStateCI.back.passOp = VK_STENCIL_OP_KEEP;
		depthStencilStateCI.back.compareOp = VK_COMPARE_OP_ALWAYS;
		depthStencilStateCI.stencilTestEnable = VK_FALSE;	// 不启用模板测试
		depthStencilStateCI.front = depthStencilStateCI.back;	// 前后两个面的模板测试状态

		// Multi sampling 状态
		VkPipelineMultisampleStateCreateInfo multisampleStateCI = {};
		multisampleStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
		multisampleStateCI.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
		multisampleStateCI.pSampleMask = nullptr;

		// 顶点输入绑定
		VkVertexInputBindingDescription vertexInputBinding = {};
		vertexInputBinding.binding = 0;
		vertexInputBinding.stride = sizeof(Vertex);
		vertexInputBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

		// 描述顶点缓冲区中的顶点属性
		std::array<VkVertexInputAttributeDescription, 2> vertexInputattributes;
		// 与顶点着色器中的layout保持一致
		// layout (location = 0) in vec3 inPos;
		// layout (location = 1) in vec3 inColor;
		vertexInputattributes[0].binding = 0;
		vertexInputattributes[0].location = 0;
		vertexInputattributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		vertexInputattributes[0].offset = offsetof(Vertex, position);
		vertexInputattributes[0].binding = 0;
		vertexInputattributes[0].location = 1;
		vertexInputattributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
		vertexInputattributes[0].offset = offsetof(Vertex, color);

		VkPipelineVertexInputStateCreateInfo vertexInputStateCI = {};
		vertexInputStateCI.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
		vertexInputStateCI.vertexBindingDescriptionCount = 1;
		vertexInputStateCI.pVertexBindingDescriptions = &vertexInputBinding;
		vertexInputStateCI.vertexAttributeDescriptionCount = 2;
		vertexInputStateCI.pVertexAttributeDescriptions = vertexInputattributes.data();

		// 着色器
		std::array<VkPipelineShaderStageCreateInfo, 2> shaderStages = {};
		// 顶点着色器
		shaderStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
		shaderStages[0].module = loadSRIPVShader(getShadersPath() + "triangle/triangle.vert.spv");
		shaderStages[0].pName = "main";		// shader的主要入口
		assert(shaderStages[0].module != VK_NULL_HANDLE);
		// 片段着色器
		shaderStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
		shaderStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
		shaderStages[1].module = loadSRIPVShader(getShadersPath() + "triangle/triangle.frag.spv");
		shaderStages[1].pName = "main";
		assert(shaderStages[1].module != VK_NULL_HANDLE);

		// 设置pipeline创建信息结构体的 shader stage
		pipelineCI.stageCount = static_cast<uint32_t>(shaderStages.size());
		pipelineCI.pStages = shaderStages.data();

		// 设置pipeline的状态
		pipelineCI.pVertexInputState = &vertexInputStateCI;
		pipelineCI.pInputAssemblyState = &inputAssemblyStateCI;
		pipelineCI.pRasterizationState = &rasterizationStateCI;
		pipelineCI.pColorBlendState = &colorBlendStateCI;
		pipelineCI.pMultisampleState = &multisampleStateCI;
		pipelineCI.pViewportState = &viewportStateCI;
		pipelineCI.pDepthStencilState = &depthStencilStateCI;
		pipelineCI.pDynamicState = &dynamicStateCI;

		VK_CHECK_RESULT(vkCreateGraphicsPipelines(device, pipelineCache, 1, &pipelineCI, nullptr, &pipeline));
		vkDestroyShaderModule(device, shaderStages[0].module, nullptr);
		vkDestroyShaderModule(device, shaderStages[1].module, nullptr);
	}

	// uniformBuffer包含相机位置、投影矩阵等在渲染过程中保持不变的常量，供着色器读取和使用
	// vulkan中使用VkDescriptorSet实现多个 uniformBuffer 和 uniformBuffer Array
	void prepareUniformBuffers()
	{

		VkBufferCreateInfo bufferCI = {};
		bufferCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCI.size = sizeof(uboVS);
		bufferCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
		VK_CHECK_RESULT(vkCreateBuffer(device, &bufferCI, nullptr, &uniformBufferVS.buffer));

		// 分配uniform buffer 的内存
		VkMemoryRequirements memReqs;
		vkGetBufferMemoryRequirements(device, uniformBufferVS.buffer, &memReqs);

		VkMemoryAllocateInfo allocInfo = {};
		allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		allocInfo.pNext = nullptr;
		allocInfo.allocationSize = memReqs.size;
		allocInfo.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		VK_CHECK_RESULT(vkAllocateMemory(device, &allocInfo, nullptr, &(uniformBufferVS.memory)));
		VK_CHECK_RESULT(vkBindBufferMemory(device, uniformBufferVS.buffer, uniformBufferVS.memory, 0));

		uniformBufferVS.descriptor.buffer = uniformBufferVS.buffer;
		uniformBufferVS.descriptor.offset = 0;
		uniformBufferVS.descriptor.range = sizeof(uboVS);

		updateUniformBuffers();
	}

	void updateUniformBuffers()
	{
		uboVS.projectionMatrix = camera.matrices.perspective;
		uboVS.modelMatrix = glm::mat4(1.0f);
		uboVS.viewMatrix = camera.matrices.view;

		uint8_t *pData;
		VK_CHECK_RESULT(vkMapMemory(device, uniformBufferVS.memory, 0, sizeof(uboVS), 0, reinterpret_cast<void **>(&pData)));
		memcpy(pData, &uboVS, sizeof(uboVS));
		vkUnmapMemory(device, uniformBufferVS.memory);
	}

	void prepare()
	{
		VulkanExampleBase::prepare();
		prepareSynchronizationPrimitives();
		prepareVertices(USE_STAGING);
		prepareUniformBuffers();
		setupDescriptorSetLayout();
		preparePipeline();
		setupDescriptorPool();
		setupDescriptorSet();
		buildCommandBuffers();
		prepared = true;
	}

	virtual void render()
	{
		if(!prepared)
			return;
		draw();
	}

	virtual void viewChanged()
	{
		updateUniformBuffers();
	}
};

// OS specific macros for the example main entry points
// Most of the code base is shared for the different supported operating systems, but stuff like message handling differs

#if defined(_WIN32)
// Windows entry point
VulkanExample *vulkanExample;
LRESULT CALLBACK WndProc(HWND hWnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	if (vulkanExample != NULL)
	{
		vulkanExample->handleMessages(hWnd, uMsg, wParam, lParam);
	}
	return (DefWindowProc(hWnd, uMsg, wParam, lParam));
}
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR pCmdLine, int nCmdShow)
{
	for (size_t i = 0; i < __argc; i++) { VulkanExample::args.push_back(__argv[i]); };
	vulkanExample = new VulkanExample();
	vulkanExample->initVulkan();
	vulkanExample->setupWindow(hInstance, WndProc);
	vulkanExample->prepare();
	vulkanExample->renderLoop();
	delete(vulkanExample);
	return 0;
}

#elif defined(__ANDROID__)
// Android entry point
VulkanExample *vulkanExample;
void android_main(android_app* state)
{
	vulkanExample = new VulkanExample();
	state->userData = vulkanExample;
	state->onAppCmd = VulkanExample::handleAppCommand;
	state->onInputEvent = VulkanExample::handleAppInput;
	androidApp = state;
	vulkanExample->renderLoop();
	delete(vulkanExample);
}
#elif defined(_DIRECT2DISPLAY)

// Linux entry point with direct to display wsi
// Direct to Displays (D2D) is used on embedded platforms
VulkanExample *vulkanExample;
static void handleEvent()
{
}
int main(const int argc, const char *argv[])
{
	for (size_t i = 0; i < argc; i++) { VulkanExample::args.push_back(argv[i]); };
	vulkanExample = new VulkanExample();
	vulkanExample->initVulkan();
	vulkanExample->prepare();
	vulkanExample->renderLoop();
	delete(vulkanExample);
	return 0;
}
#elif defined(VK_USE_PLATFORM_DIRECTFB_EXT)
VulkanExample *vulkanExample;
static void handleEvent(const DFBWindowEvent *event)
{
	if (vulkanExample != NULL)
	{
		vulkanExample->handleEvent(event);
	}
}
int main(const int argc, const char *argv[])
{
	for (size_t i = 0; i < argc; i++) { VulkanExample::args.push_back(argv[i]); };
	vulkanExample = new VulkanExample();
	vulkanExample->initVulkan();
	vulkanExample->setupWindow();
	vulkanExample->prepare();
	vulkanExample->renderLoop();
	delete(vulkanExample);
	return 0;
}
#elif defined(VK_USE_PLATFORM_WAYLAND_KHR)
VulkanExample *vulkanExample;
int main(const int argc, const char *argv[])
{
	for (size_t i = 0; i < argc; i++) { VulkanExample::args.push_back(argv[i]); };
	vulkanExample = new VulkanExample();
	vulkanExample->initVulkan();
	vulkanExample->setupWindow();
	vulkanExample->prepare();
	vulkanExample->renderLoop();
	delete(vulkanExample);
	return 0;
}
#elif defined(__linux__) || defined(__FreeBSD__)

// Linux entry point
VulkanExample *vulkanExample;
#if defined(VK_USE_PLATFORM_XCB_KHR)
static void handleEvent(const xcb_generic_event_t *event)
{
	if (vulkanExample != NULL)
	{
		vulkanExample->handleEvent(event);
	}
}
#else
static void handleEvent()
{
}
#endif
int main(const int argc, const char *argv[])
{
	for (size_t i = 0; i < argc; i++) { VulkanExample::args.push_back(argv[i]); };
	vulkanExample = new VulkanExample();
	vulkanExample->initVulkan();
	vulkanExample->setupWindow();
	vulkanExample->prepare();
	vulkanExample->renderLoop();
	delete(vulkanExample);
	return 0;
}
#elif (defined(VK_USE_PLATFORM_MACOS_MVK) && defined(VK_EXAMPLE_XCODE_GENERATED))
VulkanExample *vulkanExample;
int main(const int argc, const char *argv[])
{
	@autoreleasepool
	{
		for (size_t i = 0; i < argc; i++) { VulkanExample::args.push_back(argv[i]); };
		vulkanExample = new VulkanExample();
		vulkanExample->initVulkan();
		vulkanExample->setupWindow(nullptr);
		vulkanExample->prepare();
		vulkanExample->renderLoop();
		delete(vulkanExample);
	}
	return 0;
}
#endif