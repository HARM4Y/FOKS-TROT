[![License](https://img.shields.io/badge/License-GPLv3-blue.svg "License")](https://www.gnu.org/licenses/gpl-3.0 "License")
[![Driver](https://img.shields.io/badge/Driver-passing-green.svg "Driver")](https://github.com/hkx3upper "Driver")
[![Test](https://img.shields.io/badge/Test-passing-green.svg "Test")](https://github.com/hkx3upper "Test")
[![PR](https://img.shields.io/badge/PR-welcome-blue.svg "PR")](https://github.com/hkx3upper/FOKS-TROT/pulls "PR")
[![Issue](https://img.shields.io/badge/Issue-welcome-blue.svg "Issue")](https://github.com/hkx3upper/FOKS-TROT/issues "Issue")  
<a href="https://github.com/hkx3upper/FOKS-TROT"><img align="right" width="231" height="117" src="https://user-images.githubusercontent.com/41336794/172169698-65afd346-38c4-4fd6-861b-20940a6ee493.jpg" alt="FOKS-TROT"></a></br>
# FOKS-TROT  
## 基于Minifilter框架的双缓冲透明加解密驱动 
## ZL 202210549949.1 基于密文挪用的文件透明加解密方法及系统 专利权人 山东大学  
## It's a minifilter used for transparent-encrypting. <a href="https://github.com/hkx3upper/FOKS-TROT/wiki/Foxtrot">English</a>  

## Foreword：
**Foxtrot**是一个实验性项目，且作者对于文件系统等的理解难免会存在偏差，因此可能会产生误导，望读者辩证的学习，并且请读者遵循相关的开源协议。且本项目5月12号以前的版本已作为毕设，如有雷同，纯属雷同`((/- -)/）`  
**经过五个月的维护，**Foxtrot**迎来了第一个稳定版1.0.0.2265版，项目现在是可以稳定运行在Windows 10 x64各版本上的（应该可以），建议大家重新clone一下，不过本版本不支持之前驱动加密过的文件（见第11条）。**  
`P.S.已将项目打包成exe安装包，可以不用手动编译项目了`
## Description：
**Foxtrot**是一个使用minifilter框架的双缓冲透明加密解密过滤驱动，当进程有写入特定的文件扩展名（比如txt，docx）文件的倾向时自动加密文件。授权进程想要读取密文文件时自动解密，非授权进程不解密，显示密文，且不允许修改密文。
桌面端可以发送特权加密和特权解密命令，实现单独加密或解密文件；或者配置进程权限，机密文件夹，需加密的文件类型。  
```
1.本项目使用双缓冲，授权进程和非授权进程分别使用明文缓冲和密文缓冲；  
2.使用StreamContext存放驱动运行时的文件信息，在文件的尾部使用4KB文件标识尾储存解密所需信息；  
3.使用AES-128 ECB模式，16个字节以内分别在SetInfo->EOF和WriteCachedIo时扩展文件大小到16字节，
  大于16个字节，使用密文挪用（Ciphertext stealing）的方法，避免明文必须分块对齐的问题；  
4.Write和Read使用SwapBuffers的方式进行透明加密解密；  
5.特权加密和特权解密使用重入（Reentry）的方式，使驱动加密解密文件；  
6.在FileRenameInformationEx和FileRenameInformation重命名操作时做处理，
  可以自动加密解密docx，doc，pptx，ppt，xlsx，xls等使用tmp文件重命名方式读写的文件；  
7.注册进程相关回调，使用链表统一管理授权与非授权进程；
  注册进程与线程对象回调，保护进程EPROCESS,ETHREAD对象；对授权进程的代码段进行完整性校验。  
8.设置机密文件夹，文件处于该文件夹下才会透明加密，
  并可以从桌面PocUser配置机密文件夹与需管控的文件扩展名 @wangzhankun  
9.PostOperation统一使用函数FltDoCompletionProcessingWhenSafed（PostRead除外），
  InstanceSetup时使用Dpc+WorkItem回调（封装为PocDoCompletionProcessingWhenSafe），
  避免在DISPATCH_LEVEL时出现IRQL_NOT_LESS_OR_EQUAL之类的蓝屏；  
10.PostClose时使用单独的线程，等待操作该文件的所有授权进程结束以后，
  再重入加密或写入文件标识尾，解决了docx类文件的死锁问题；  
11.将ULONG改成LONGLONG，原则上可以支持4GB以上文件（目前特权加密和特权解密暂不支持4GB以上文件，  
  而且在内存有限的情况下，特权解密有可能会因非分页内存的缺少而失败，不想写了，这里可以放循环里读写大文件） 
12.用WPF写了用户界面，可以配置授权进程，需管控的文件类型，机密文件夹，以及特权加密、解密文件；  
  使用InstallShield打包安装包；  
13.增加进程权限：备份权限进程，比如VMTools和explorer.exe，可以将完整的密文文件从虚拟机中拷贝出，或者  
  从机密文件夹向外拷贝密文文件，和粘贴文件到机密文件夹（未加密的自动加密，已加密的重复加密后自动解密一次）    
14.在Write加密以后，ObDereferenceObject一个之前建好的FileObject，触发Close创建线程准备写入尾或重入加密；  
15.允许驱动特权加密或解密机密文件夹以外的文件；驱动卸载时清除明文缓冲，防止明文泄露；  
16.WRITE_THROUGH时，Ntfs并不会在PagingIo之前更新Fcb->FileSize，它使用TopLevelIrpContext + 184截断  
  PagingIo的数据，在CachedIo返回时更新该FileSize，所以这里使用SC->WriteThroughFileSize暂时保存FileSize。    
```
## Build & Installation：
1.建议在Windows 10 x64，NTFS环境运行  
```
已测试系统及软件版本:  
Windows 10 x64 1809(17763.2928) LTSC 企业版 [WPS 11.1.0.11365]  
Windows 10 x64 1903(18362.30) 教育版 [Microsoft Office Professional Plus 2021 x64] 
                                    [WPS 11.1.0.11744] [360安全卫士 15.0.0.1061]
Windows 10 x64 1909(18363.592) 教育版  [WPS 11.1.0.11744]  
Windows Server 2019 DataCenter 1809(17763.379)  
```
2.系统开启测试模式，cmd以管理员身份运行，输入`bcdedit /set testsigning on`后重启电脑  
3.驱动日志输出（可选）  
```
找到注册表项：HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\Session Manager\Debug Print Filter  
没有Debug Print Filter就新建，在这个键下新建dword值 “default”，十六进制为0xF，然后重启电脑  
以管理员身份运行DebugView，设置`Capture->Capture Kernel`显示驱动日志  
```
4.下载安装包（可选）  
  
[![Download](https://img.shields.io/badge/Download-5.09MB-green.svg "Download")](https://github.com/hkx3upper/FOKS-TROT/releases/ "Download")  
`如果下载安装包，那就不需要手动编译项目了，可以直接跳到第10步`  
5.安装并导入CNG库  
```
https://www.microsoft.com/en-us/download/details.aspx?id=30688  
需要在微软官网下载Cryptographic Provider Development Kit,  
项目->属性的VC++目录的包含目录，库目录设置相应的位置  
链接器的常规->附加库目录C:\Windows Kits\10\Cryptographic Provider Development Kit\Lib\x64  
输入->附加依赖项ksecdd.lib
```
6.在`Config.c`中设置目标文件扩展名，机密文件夹，以及授权进程，备份进程  
7.使用Visual Studio 2019编译Debug x64 Poc驱动，UserDll（可选）和UserPanel（可选）  
8.鼠标右键菜单增加特权加密和特权解密功能（可选）  
```
这个功能可以直接用鼠标右键选择一个文件，然后点击特权加密或特权解密，不需要cmd命令行了
新建注册表项：HKEY_CLASSES_ROOT\*\shell\Encrypt，将这个键的“默认”的数据改为特权加密
新建注册表项：HKEY_CLASSES_ROOT\*\shell\Encrypt\command，
将这个键的“默认”值的数据改为 "路径\PocUserPanel.exe" 8 "%1"

新建注册表项：HKEY_CLASSES_ROOT\*\shell\Decrypt，将这个键的“默认”的数据改为特权解密
新建注册表项：HKEY_CLASSES_ROOT\*\shell\Decrypt\command，
将这个键的“默认”值的数据改为 "路径\PocUserPanel.exe" 4 "%1"
``` 
9.如果使用OsrLoader之类的加载器加载驱动，Type请选择Minifilter，也可以使用cmd加载，如下  
```
::加载
rundll32.exe setupapi.dll,InstallHinfSection DefaultInstall 132 路径\Poc.inf
net start Poc
pause
::卸载
net stop Poc
rundll32.exe setupapi.dll,InstallHinfSection DefaultUninstall 132 路径\Poc.inf
```
10.驱动加载以后，测试是否正常工作（详见TestManual.md）  
```
用notepad.exe（默认配置为授权进程）写入txt文件一些数据，
然后使用wordpad.exe（默认配置为非授权进程）打开，只能看到杂乱的数据，说明加密成功。  
P.S. 文件加密以后，加密标识尾只有在关机重启且不开驱动的情况下才能看到。  
```
![bandicam-2022-06-07-14-17-43-706](https://user-images.githubusercontent.com/41336794/172311235-59075006-aa5e-42f1-a6c4-c976785e6f5a.gif)  
  
11.使用编译好的或安装包中的PocUserPanel配置各种参数  
</br><img src="https://user-images.githubusercontent.com/41336794/173342125-2198e70f-8590-4002-ab7f-5dc5ef899720.JPG"></a></br>  
## Unfixed & Prospect：
1.授权进程"另存为"会导致明文泄露，这里可以在PreCreate判断一下是否是授权进程+有写入倾向，是，则不过滤扩展名以及路径，让文件进入驱动控制。  
2.将文件标识尾改成文件标识头的方式，防止意外断电的情况。  
**当初写这个项目时，我对Windows内核懂得不多，所以有很多的地方从一开始架构时就有缺陷，比如密文挪用这部分，它和扩展文件大小的方法混在了一起，另外写入文件尾这一块也很笨拙，为了密文挪用这点“创新”，我妥协了很多，这导致本来可以简化的地方搞得很复杂。  
我希望这个项目成为各位同僚开发时的参考，而非一个直接用的产品，双缓冲部分的实现，文件大小的扩展等等，都是值得参考的。另外我把论文和开发文档也放在了项目里，相信会有助于各位对这个项目的理解。**  
## Credits & References：
hkx3upper:(<a href="https://github.com/hkx3upper">@hkx3upper</a>)  
wangzhankun:(<a href="https://github.com/wangzhankun">@wangzhankun</a>)  
MaterialDesignInXAML:(<a href="https://github.com/MaterialDesignInXAML">@MaterialDesignInXAML</a>)  
CSharpDesignPro:(<a href="https://github.com/CSharpDesignPro/WPF---MVVM-Based-Modern-Dashboard">@CSharpDesignPro</a>)  
Windows-driver-samples\filesys\fastfat:(<a href="https://github.com/microsoft/Windows-driver-samples/tree/main/filesys">Fastfat</a>)  
Windows-driver-samples\filesys\minifilter:(<a href="https://github.com/microsoft/Windows-driver-samples/tree/main/filesys">Minifilter</a>)  
Windows-Research-Kernel:(<a href="https://github.com/HighSchoolSoftwareClub/Windows-Research-Kernel-WRK-">WRK</a>)  
WinNT4:(<a href="https://github.com/ZoloZiak/WinNT4">WinNT4</a>)  
基于Minifilter微框架的文件加解密系统的设计与实现 何明 2014.06  
基于双缓冲过滤驱动的透明加密系统研究与实现 刘晗 2010.04  
基于 IBE 和 FUSE 的双向透明文件加密系统的研究与实现 何翔 2017.04  
Windows NT File System Internals - A Developer's  Guide  
Windows内核安全与驱动开发 谭文  
Windows内核情景分析 毛德操  
Windows内核原理与实现 潘爱民  
## License：
**Foxtrot**, and all its submodules and repos, unless a license is otherwise specified, are licensed under **GPLv3** LICENSE.  
Dependencies are licensed by their own.  
