#include "gl-headers.h"
#include "options.h"
#include "log.h"
#include "util.h"

#include "canvas-generic.h"
#include "native-state-ohos.h"
#include "gl-state-egl.h"

using std::vector;
using std::string;

int main(int argc, char *argv[])
{
    if (!Options::parse_args(argc, argv))
        return 1;

    /* Initialize Log class */
    Log::init(Util::appname_from_path(argv[0]), Options::show_debug);

    if (Options::show_help) {
        Options::print_help();
        return 0;
    }

    /* Force 320x240 output for validation */
    if (Options::validate &&
        Options::size != std::pair<int,int>(320, 240))
    {
        Log::info("Ignoring custom size %dx%d for validation. Using 800x600.\n",
                  Options::size.first, Options::size.second);
        Options::size = std::pair<int,int>(320, 240);
    }

    // Create the canvas
    NativeStateOhos native_state;

    GLStateEGL gl_state;

    CanvasGeneric canvas(native_state, gl_state, Options::size.first, Options::size.second);

    canvas.offscreen(Options::offscreen);

    canvas.visual_config(Options::visual_config);

    if (!canvas.init()) {
        Log::error("%s: Could not initialize canvas\n", __FUNCTION__);
        return 1;
    }

    Log::info("=======================================================\n");
    canvas.print_info();
    Log::info("=======================================================\n");

    canvas.visible(true);

     /**
      ** 数据处理: 生成和绑定VBO, 设置属性指针
      **/
    // 三角形的顶点数据, 规范化(x,y,z)都要映射到[-1,1]之间
    const float triangle[] = {
        // 位置
        -0.5f, -0.5f, 0.0f,  // 左下
         0.5f, -0.5f, 0.0f,  // 右下
         0.0f,  0.5f, 0.0f   // 正上
    };

    // 生成并绑定立方体的VBO
    GLuint vertex_buffer_object; // VBO
    glGenBuffers(1, &vertex_buffer_object);
    glBindBuffer(GL_ARRAY_BUFFER, vertex_buffer_object);

    // 将顶点数据绑定到当前默认的缓冲中, 好处是不用将顶点数据一个一个地发送到显卡上, 可以借助VBO一次性发送所有顶点数据
    // GL_STATIC_DRAW表示顶点数据不会被改变
    glBufferData(GL_ARRAY_BUFFER, sizeof(triangle), triangle, GL_STATIC_DRAW);

    // 设置顶点属性指针
    // 第一个参数0: 顶点着色器的位置值
    // 第二个参数3: 位置属性是一个三分量的向量
    // 第三个参数: 顶点的类型
    // 第四个参数: 是否希望数据标准化,映射到[0,1]
    // 第五个参数: 步长,表示连续顶点属性之间的间隔,下一组的数据再3个float之后
    // 第六个参数: 数据的偏移量, 位置属性在开头, 因此为0, 还需要强制类型转换
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(0); // 开启0通道

    glBindBuffer(GL_ARRAY_BUFFER, 0);

    /**
     ** 着色器: 顶点和片段着色器
     **/
    // 着色器源码 -> 生成并编译着色器 -> 链接着色器到着色器程序 -> 删除着色器
    const char *vertex_shader_source =
            "attribute vec4 a_Position;\n" // 位置变量属性设置为0
            "void main()\n"
            "{\n"
            "    gl_Position = a_Position;\n"
            "}\n\0";

    // 设置片元像素的颜色为红色, vec4(r,g,b,a)
    const char *fragment_shader_source =
            "void main()\n"
            "{\n"
            "    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n"
            "}\n\0";

    // 生成并编译着色器
    // 顶点着色器
    int vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vertex_shader, 1, &vertex_shader_source, NULL);
    glCompileShader(vertex_shader);

    int success;
    char info_log[512];

    // 检查着色器是否成功编译, 如果编译失败, 打印错误信息
    glGetShaderiv(vertex_shader, GL_COMPILE_STATUS, &success);
    if(!success){
        glGetShaderInfoLog(vertex_shader, 512, NULL, info_log);
        Log::error("SHADER::VERTEX::COMPILATION_FAIILED %s\n", info_log);
    }

    // 片元着色器
    int fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fragment_shader, 1, &fragment_shader_source, NULL);
    glCompileShader(fragment_shader);

    // 检查着色器是否成功编译, 如果编译失败, 打印错误信息
    glGetShaderiv(fragment_shader, GL_COMPILE_STATUS, &success);
    if(!success){
        glGetShaderInfoLog(fragment_shader, 512, NULL, info_log);
        Log::error("SHADER::FRAGMENT::COMPILATION_FAIILED %s\n", info_log);
    }

    // 链接顶点和片段着色器至一个着色器程序
    int shader_program = glCreateProgram();
    glAttachShader(shader_program, vertex_shader);
    glAttachShader(shader_program, fragment_shader);

    glLinkProgram(shader_program);

    // 检查着色器是否成功链接, 如果链接失败, 打印错误信息
    glGetProgramiv(shader_program, GL_LINK_STATUS, &success);
    if(!success){
        glGetProgramInfoLog(shader_program, 512, NULL, info_log);
        Log::error("SHADER::PROGRAM::LINKING_FAIILED %s\n", info_log);
    }

    // 删除顶点和片段着色器
    glDeleteShader(vertex_shader);
    glDeleteShader(fragment_shader);

    /**
     ** 渲染
     **/
    // 清空颜色缓冲
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);   // 用黑色背景色来清空
    glClear(GL_COLOR_BUFFER_BIT);

    // 使用着色器程序
    glUseProgram(shader_program);

    // 绘制三角形
    glDrawArrays(GL_TRIANGLES, 0, 3); // 绘制三角形, 绘制三角形, 顶点起始索引值, 绘制数量

    // 更新交换缓冲
    canvas.update();

    /**
     ** 善后工作
     **/
    glDeleteBuffers(1, &vertex_buffer_object);

    getchar();

    return 0;
}
