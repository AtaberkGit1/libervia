# LiberVia

Ultra hafif, tek butonlu Windows DPI bypass aracı ve Android istemcisi.

- **Web arayüzü** (`src/`): React 19 + Vite + Tailwind CSS
- **Android** (`android/`): Kotlin + NDK (C11 motor), VpnService tabanlı
- Windows tarafı Win32 + WinDivert kullanır

## Web arayüzü

```bash
npm install
npm run dev     # http://localhost:3000
npm run build
```

## Android

```bash
cd android
./gradlew assembleDebug
```

## Notlar

- `tools/`, `LiberVia-Win64/` ve büyük ikili dosyalar (`.exe`, `.zip`, `.apk`) depoya dahil edilmez.
- `node_*.json`, `nodes.txt` gibi kimlik bilgisi içeren yapılandırmalar `.gitignore` ile hariç tutulur; kendi `node_0.json` dosyanızı `.env.example` benzeri bir şablon üzerinden oluşturun.
