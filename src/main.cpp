#include <algorithm>
#include <vulkan/vulkan.h>
#include "Instance.h"
#include "Window.h"
#include "Renderer.h"
#include "Camera.h"
#include "Scene.h"
#include "Terrain.h"

Device* device;
SwapChain* swapChain;
Renderer* renderer;
Camera* camera;

namespace {
    // the default fly speed crosses the terrain in this time
    const float SECONDS_TO_CROSS_TERRAIN = 8.0f;
    // the longest fly step a stalled frame may take
    const float MAX_FRAME_SECONDS = 0.25f;

    bool leftMouseDown = false;
    bool rightMouseDown = false;
    bool middleMouseDown = false;
    double lastX = 0.0;
    double lastY = 0.0;

    void resizeCallback(GLFWwindow* window, int width, int height) {
        if (width == 0 || height == 0) return;

        vkDeviceWaitIdle(device->GetVkDevice());
        swapChain->Recreate();
        renderer->RecreateFrameResources();
        camera->SetViewport(width, height);
    }

    bool altHeld(GLFWwindow* window) {
        return glfwGetKey(window, GLFW_KEY_LEFT_ALT) == GLFW_PRESS || glfwGetKey(window, GLFW_KEY_RIGHT_ALT) == GLFW_PRESS;
    }

    // keys are polled every frame so a held key moves smoothly; not with Alt, where RMB dollies
    void flyFromKeys(GLFWwindow* window, float seconds) {
        if (!rightMouseDown || altHeld(window)) {
            return;
        }
        auto held = [window](int key) { return glfwGetKey(window, key) == GLFW_PRESS ? 1 : 0; };
        camera->Fly(held(GLFW_KEY_W) - held(GLFW_KEY_S), held(GLFW_KEY_D) - held(GLFW_KEY_A),
            held(GLFW_KEY_E) - held(GLFW_KEY_Q), seconds);
    }

    void keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
        if (action != GLFW_PRESS) {
            return;
        }
        if (key == GLFW_KEY_ESCAPE) {
            glfwSetWindowShouldClose(window, GLFW_TRUE);
        }
        // not while flying, where F sits next to the movement keys
        else if (key == GLFW_KEY_F && !rightMouseDown) {
            camera->ResetToHome();
        }
    }

    void mouseDownCallback(GLFWwindow* window, int button, int action, int mods) {
        bool press = action == GLFW_PRESS;
        switch (button) {
            case GLFW_MOUSE_BUTTON_LEFT:
                leftMouseDown = press;
                break;
            case GLFW_MOUSE_BUTTON_RIGHT:
                rightMouseDown = press;
                break;
            case GLFW_MOUSE_BUTTON_MIDDLE:
                middleMouseDown = press;
                break;
            default:
                return;
        }
        bool dragging = leftMouseDown || rightMouseDown || middleMouseDown;
        glfwSetInputMode(window, GLFW_CURSOR, dragging ? GLFW_CURSOR_DISABLED : GLFW_CURSOR_NORMAL);
        glfwGetCursorPos(window, &lastX, &lastY);
    }

    void mouseMoveCallback(GLFWwindow* window, double xPosition, double yPosition) {
        double dx = xPosition - lastX;
        double dy = yPosition - lastY;
        lastX = xPosition;
        lastY = yPosition;
        if (dx == 0.0 && dy == 0.0) {
            return;
        }

        bool alt = altHeld(window);
        if (alt && leftMouseDown) {
            camera->Orbit(dx, dy);
        } else if (alt && rightMouseDown) {
            camera->DollyToPivot(dx, dy);
        } else if (middleMouseDown || (leftMouseDown && rightMouseDown)) {
            camera->Pan(dx, dy);
        } else if (rightMouseDown) {
            camera->Turn(dx, dy);
        } else if (leftMouseDown) {
            camera->MoveAlongGround(dx, dy);
        }
    }

    void scrollCallback(GLFWwindow* window, double xOffset, double yOffset) {
        if (rightMouseDown) {
            camera->ScaleFlySpeed(static_cast<float>(yOffset));
        } else {
            camera->MoveAlongView(static_cast<float>(yOffset));
        }
    }
}

int main() {
    static constexpr char* applicationName = "Vulkan Grass Rendering";
    static constexpr int windowWidth = 1920;
    static constexpr int windowHeight = 1080;
    InitializeWindow(windowWidth, windowHeight, applicationName);

    unsigned int glfwExtensionCount = 0;
    const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

    Instance* instance = new Instance(applicationName, glfwExtensionCount, glfwExtensions);

    VkSurfaceKHR surface;
    if (glfwCreateWindowSurface(instance->GetVkInstance(), GetGLFWWindow(), nullptr, &surface) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create window surface");
    }

    instance->PickPhysicalDevice({ VK_KHR_SWAPCHAIN_EXTENSION_NAME }, QueueFlagBit::GraphicsBit | QueueFlagBit::TransferBit | QueueFlagBit::ComputeBit | QueueFlagBit::PresentBit, surface);

    VkPhysicalDeviceFeatures deviceFeatures = {};
    deviceFeatures.tessellationShader = VK_TRUE;
    deviceFeatures.fillModeNonSolid = VK_TRUE;
    deviceFeatures.samplerAnisotropy = VK_TRUE;

    device = instance->CreateDevice(QueueFlagBit::GraphicsBit | QueueFlagBit::TransferBit | QueueFlagBit::ComputeBit | QueueFlagBit::PresentBit, deviceFeatures);

    swapChain = device->CreateSwapChain(surface, 5);

    camera = new Camera(device, windowWidth, windowHeight);

    VkCommandPoolCreateInfo transferPoolInfo = {};
    transferPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    transferPoolInfo.queueFamilyIndex = device->GetInstance()->GetQueueFamilyIndices()[QueueFlags::Transfer];
    transferPoolInfo.flags = 0;

    VkCommandPool transferCommandPool;
    if (vkCreateCommandPool(device->GetVkDevice(), &transferPoolInfo, nullptr, &transferCommandPool) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create command pool");
    }

    // 128 m square, 0.5 m cells
    Terrain terrain(128.0f, 256);
    std::vector<Vertex> terrainVertices;
    std::vector<uint32_t> terrainIndices;
    terrain.BuildMesh(terrainVertices, terrainIndices);
    Model* ground = new Model(device, transferCommandPool, terrainVertices, terrainIndices);

    Blades* blades = new Blades(device, transferCommandPool, terrain);

    glm::vec3 center(0.0f, terrain.HeightAt(0.0f, 0.0f), 0.0f);
    camera->SetHome(center + glm::vec3(0.0f, 11.0f, 28.0f), center);
    camera->SetFlySpeed(terrain.GetSize() / SECONDS_TO_CROSS_TERRAIN);

    vkDestroyCommandPool(device->GetVkDevice(), transferCommandPool, nullptr);

    Scene* scene = new Scene(device);
    scene->AddModel(ground);
    scene->AddBlades(blades);

    renderer = new Renderer(device, swapChain, scene, camera);

    glfwSetWindowSizeCallback(GetGLFWWindow(), resizeCallback);
    glfwSetMouseButtonCallback(GetGLFWWindow(), mouseDownCallback);
    glfwSetCursorPosCallback(GetGLFWWindow(), mouseMoveCallback);
    glfwSetScrollCallback(GetGLFWWindow(), scrollCallback);
    glfwSetKeyCallback(GetGLFWWindow(), keyCallback);

    double lastFrameTime = glfwGetTime();
    while (!ShouldQuit()) {
        glfwPollEvents();
        double now = glfwGetTime();
        flyFromKeys(GetGLFWWindow(), static_cast<float>(std::min(now - lastFrameTime, static_cast<double>(MAX_FRAME_SECONDS))));
        lastFrameTime = now;
        scene->UpdateTime();
        renderer->Frame();
    }

    vkDeviceWaitIdle(device->GetVkDevice());

    delete scene;
    delete ground;
    delete blades;
    delete camera;
    delete renderer;
    delete swapChain;
    delete device;
    delete instance;
    DestroyWindow();
    return 0;
}
