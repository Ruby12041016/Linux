`std::filesystem` 是 C++17 提供的：

# 文件系统库

专门用来：

* 判断文件是否存在
* 创建目录
* 删除文件
* 遍历目录
* 获取文件大小
* 处理路径
* 改名
* 复制文件

等等。

以前这些操作：

Linux 要用：

```cpp id="p1l9pf"
access()
stat()
mkdir()
opendir()
```

Windows 又是另一套 API。

非常麻烦。

---

# 现在 C++17 统一了

直接：

```cpp id="a4u6eh"
#include <filesystem>
```

即可。

命名空间：

```cpp id="e5w44z"
std::filesystem
```

一般简写：

```cpp id="kq5gn8"
namespace fs = std::filesystem;
```

---

# 用它干嘛？


```cpp id="l51t4q"
fs::exists(filename)
```

意思是：

```text id="aqx7vf"
判断文件是否存在
```

例如：

```cpp id="gcv6q5"
if (fs::exists("test.txt")) {
    std::cout << "文件存在";
}
```

---

# 还有两个很好用的东西

## 1. stem()

```cpp id="3jlwm9"
p.stem()
```

获取：

```text id="i4jb4f"
文件主名
```

例如：

```text id="b0x2tx"
hello.txt
```

得到：

```text id="p44k1u"
hello
```

---

## 2. extension()

```cpp id="a1n9wy"
p.extension()
```

获取：

```text id="ot5qos"
扩展名
```

例如：

```text id="0a0pjt"
.txt
```

---

# 例子

```cpp id="jlwmxg"
fs::path p("hello.txt");

std::cout << p.stem();      
```

输出：

```text id="zw6x2v"
hello
```

---

```cpp id="0yvlg3"
std::cout << p.extension();
```

输出：

```text id="00agqf"
.txt
```

---

# 为什么不用字符串硬切？

因为：

```text id="9zic5j"
a.tar.gz
```

这种：

* 哪部分是后缀？
* 哪部分是文件名？

自己切容易出 bug。

用filesystem 可以自动处理好。

---

# 目录方面

## 判断目录

```cpp id="0iq18g"
fs::is_directory(path)
```

---

## 创建下载目录

```cpp id="1plvqr"
fs::create_directory("downloads")
```

---

## 获取文件大小

```cpp id="j5xxy0"
fs::file_size(path)
```

---

## 遍历目录

```cpp id="u42yye"
for (auto& p : fs::directory_iterator("."))
```

---

# 注意

必须：

## 编译开启 C++17

```bash id="x49c4w"
g++ main.cpp -std=c++17
```

否则：

```text id="mb2rpn"
filesystem not found
```

---

# 老版本 g++

有时还需要：

```bash id="0s9uqs"
-lstdc++fs
```

例如：

```bash id="ylnw8t"
g++ main.cpp -std=c++17 -lstdc++fs
```

但：

* gcc 9+
* clang 新版

一般不用了。
