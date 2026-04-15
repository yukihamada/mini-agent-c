import SwiftUI
import WebKit

struct ContentView: View {
    @StateObject private var vm = WebViewModel()

    var body: some View {
        ZStack {
            Color(red: 0.05, green: 0.05, blue: 0.08).ignoresSafeArea()
            WebView(vm: vm)
                .ignoresSafeArea()
            if vm.isLoading {
                SplashView()
            }
        }
        .preferredColorScheme(.dark)
    }
}

// MARK: – WebView

struct WebView: UIViewRepresentable {
    @ObservedObject var vm: WebViewModel

    func makeUIView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.allowsInlineMediaPlayback = true
        config.mediaTypesRequiringUserActionForPlayback = []

        // Message handler to receive token requests from JS
        let controller = WKUserContentController()
        controller.add(context.coordinator, name: "evoApp")
        config.userContentController = controller

        let wv = WKWebView(frame: .zero, configuration: config)
        wv.navigationDelegate = context.coordinator
        wv.allowsBackForwardNavigationGestures = false
        wv.scrollView.bounces = false
        wv.backgroundColor = UIColor(red: 0.05, green: 0.05, blue: 0.08, alpha: 1)
        wv.isOpaque = false
        vm.webView = wv

        // Inject auth token on every page load
        let script = WKUserScript(
            source: injectScript(),
            injectionTime: .atDocumentEnd,
            forMainFrameOnly: true
        )
        controller.addUserScript(script)

        wv.load(URLRequest(url: URL(string: Config.serverURL)!))
        return wv
    }

    func updateUIView(_ uiView: WKWebView, context: Context) {}

    func makeCoordinator() -> Coordinator { Coordinator(vm: vm) }

    private func injectScript() -> String {
        let token = Config.authToken
        return """
        (function() {
            // Auto-fill auth token stored in app config
            var TOKEN = '\(token)';
            if (TOKEN) {
                localStorage.setItem('mac_token', TOKEN);
                var inp = document.getElementById('auth-token');
                if (inp) inp.value = TOKEN;
            }
            // Notify native app when loaded
            if (window.webkit && window.webkit.messageHandlers.evoApp) {
                window.webkit.messageHandlers.evoApp.postMessage({type: 'loaded'});
            }
        })();
        """
    }

    class Coordinator: NSObject, WKNavigationDelegate, WKScriptMessageHandler {
        let vm: WebViewModel

        init(vm: WebViewModel) { self.vm = vm }

        func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) {
                withAnimation(.easeOut(duration: 0.4)) { self.vm.isLoading = false }
            }
        }

        func webView(_ webView: WKWebView, didFail navigation: WKNavigation!, withError error: Error) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                withAnimation { self.vm.isLoading = false }
                self.vm.loadError = error.localizedDescription
            }
        }

        func webView(_ webView: WKWebView, didFailProvisionalNavigation navigation: WKNavigation!, withError error: Error) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                withAnimation { self.vm.isLoading = false }
                self.vm.loadError = error.localizedDescription
            }
        }

        func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
            // Handle messages from web page JS if needed
        }
    }
}

// MARK: – ViewModel

@MainActor
class WebViewModel: ObservableObject {
    @Published var isLoading = true
    @Published var loadError: String? = nil
    weak var webView: WKWebView?

    func reload() {
        isLoading = true
        loadError = nil
        webView?.reload()
    }
}

// MARK: – Splash screen

struct SplashView: View {
    @State private var pulse = false

    var body: some View {
        ZStack {
            Color(red: 0.05, green: 0.05, blue: 0.08).ignoresSafeArea()
            VStack(spacing: 20) {
                ZStack {
                    Circle()
                        .fill(Color(red: 0.06, green: 0.73, blue: 0.51).opacity(0.15))
                        .frame(width: 90, height: 90)
                        .scaleEffect(pulse ? 1.15 : 1.0)
                        .animation(.easeInOut(duration: 1.2).repeatForever(autoreverses: true), value: pulse)
                    Image(systemName: "arrow.triangle.2.circlepath")
                        .font(.system(size: 36, weight: .light))
                        .foregroundColor(Color(red: 0.06, green: 0.73, blue: 0.51))
                }
                Text("Evo")
                    .font(.system(size: 32, weight: .bold, design: .rounded))
                    .foregroundColor(.white)
                Text("Self-evolving agent")
                    .font(.subheadline)
                    .foregroundColor(.gray)
            }
        }
        .onAppear { pulse = true }
    }
}
