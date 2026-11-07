@echo off
chcp 65001 >nul
echo ===================================================
echo     KHOI DONG WEB APP DIEU KHIEN ROBOT ARM MQTT
echo ===================================================
echo.
echo Dang mo Web App tren trinh duyet...
start "" "http://localhost:8080/index.html"
echo.
echo Dang khoi dong Web Server cuc bo tai http://localhost:8080
echo (Nhan Ctrl+C de dung server khi khong su dung nua)
echo.
cd /d "%~dp0"
python -m http.server 8080
if %errorlevel% neq 0 (
    echo Khong tim thay Python, dang mo truc tiep file HTML...
    start "" "%~dp0index.html"
)
pause
