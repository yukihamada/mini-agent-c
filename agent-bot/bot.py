#!/usr/bin/env python3
"""
agent-bot: Telegram + LINE interface for agent.v10
Handles Telegram via long-polling, LINE via Flask webhook
"""
import os
import sys
import subprocess
import threading
import time
from flask import Flask, request, jsonify
import requests

# Configuration from environment
TELEGRAM_TOKEN = os.getenv('TELEGRAM_BOT_TOKEN', '')
LINE_CHANNEL_SECRET = os.getenv('LINE_CHANNEL_SECRET', '')
LINE_CHANNEL_ACCESS_TOKEN = os.getenv('LINE_CHANNEL_ACCESS_TOKEN', '')
AUTHORIZED_USERS = os.getenv('AUTHORIZED_USERS', '').split(',')
AGENT_PATH = '/Users/yuki/workspace/mini-agent-c/agent.v10'

# Flask app for LINE webhook
app = Flask(__name__)

def run_agent(task):
    """Execute agent.v10 with the given task and return output"""
    try:
        result = subprocess.run(
            [AGENT_PATH, task],
            cwd='/Users/yuki/workspace/mini-agent-c',
            capture_output=True,
            text=True,
            timeout=300  # 5 minute timeout
        )
        output = result.stdout if result.stdout else result.stderr
        if not output:
            output = f"Agent exited with code {result.returncode}"
        # Truncate if too long (Telegram has 4096 char limit)
        if len(output) > 4000:
            output = output[:3900] + "\n\n... (truncated)"
        return output
    except subprocess.TimeoutExpired:
        return "⏱ Agent execution timeout (5 minutes)"
    except Exception as e:
        return f"❌ Error running agent: {str(e)}"

def is_authorized(user_id):
    """Check if user is authorized"""
    if not AUTHORIZED_USERS or AUTHORIZED_USERS == ['']:
        return True  # If no restriction, allow all
    return str(user_id) in AUTHORIZED_USERS

# ============= TELEGRAM BOT =============

def send_telegram_message(chat_id, text):
    """Send message via Telegram API"""
    url = f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/sendMessage"
    try:
        requests.post(url, json={'chat_id': chat_id, 'text': text}, timeout=30)
    except Exception as e:
        print(f"Error sending Telegram message: {e}")

def handle_telegram_update(update):
    """Process a Telegram update"""
    try:
        if 'message' not in update:
            return
        
        message = update['message']
        chat_id = message['chat']['id']
        user_id = message['from']['id']
        
        if 'text' not in message:
            return
        
        text = message['text'].strip()
        
        # Check authorization
        if not is_authorized(user_id):
            send_telegram_message(chat_id, "⛔️ Unauthorized. Contact admin.")
            return
        
        # Handle /start command
        if text == '/start':
            send_telegram_message(chat_id, 
                "🤖 Agent.v10 Bot\n\n"
                "Send me any task and I'll execute it with agent.v10\n\n"
                f"Your ID: {user_id}"
            )
            return
        
        # Execute agent
        send_telegram_message(chat_id, f"🔄 Executing: {text}")
        result = run_agent(text)
        send_telegram_message(chat_id, result)
        
    except Exception as e:
        print(f"Error handling Telegram update: {e}")

def telegram_polling():
    """Long-polling loop for Telegram"""
    if not TELEGRAM_TOKEN:
        print("⚠️  TELEGRAM_BOT_TOKEN not set, Telegram bot disabled")
        return
    
    print("🚀 Telegram bot starting (long-polling)...")
    offset = 0
    
    while True:
        try:
            url = f"https://api.telegram.org/bot{TELEGRAM_TOKEN}/getUpdates"
            params = {'offset': offset, 'timeout': 30}
            response = requests.get(url, params=params, timeout=35)
            
            if response.status_code != 200:
                print(f"Telegram API error: {response.status_code}")
                time.sleep(5)
                continue
            
            data = response.json()
            if not data.get('ok'):
                print(f"Telegram API not ok: {data}")
                time.sleep(5)
                continue
            
            updates = data.get('result', [])
            for update in updates:
                offset = max(offset, update['update_id'] + 1)
                handle_telegram_update(update)
            
            if not updates:
                time.sleep(1)
                
        except requests.exceptions.Timeout:
            # Expected during long polling, continue
            continue
        except Exception as e:
            print(f"Telegram polling error: {e}")
            time.sleep(5)

# ============= LINE BOT =============

@app.route('/webhook/line', methods=['POST'])
def line_webhook():
    """Handle LINE webhook"""
    try:
        if not LINE_CHANNEL_ACCESS_TOKEN:
            return jsonify({'status': 'LINE not configured'}), 200
        
        data = request.json
        
        # LINE webhook verification
        if not data or 'events' not in data:
            return jsonify({'status': 'ok'}), 200
        
        for event in data['events']:
            if event['type'] != 'message' or event['message']['type'] != 'text':
                continue
            
            reply_token = event['replyToken']
            user_id = event['source']['userId']
            text = event['message']['text'].strip()
            
            # Check authorization
            if not is_authorized(user_id):
                reply_line_message(reply_token, "⛔️ Unauthorized. Contact admin.")
                continue
            
            # Execute agent
            result = run_agent(text)
            reply_line_message(reply_token, result)
        
        return jsonify({'status': 'ok'}), 200
        
    except Exception as e:
        print(f"LINE webhook error: {e}")
        return jsonify({'status': 'error'}), 500

def reply_line_message(reply_token, text):
    """Reply to LINE message"""
    url = "https://api.line.me/v2/bot/message/reply"
    headers = {
        'Content-Type': 'application/json',
        'Authorization': f'Bearer {LINE_CHANNEL_ACCESS_TOKEN}'
    }
    data = {
        'replyToken': reply_token,
        'messages': [{'type': 'text', 'text': text[:5000]}]  # LINE limit
    }
    try:
        requests.post(url, json=data, headers=headers, timeout=10)
    except Exception as e:
        print(f"Error replying to LINE: {e}")

@app.route('/health', methods=['GET'])
def health():
    """Health check endpoint"""
    return jsonify({
        'status': 'ok',
        'telegram_enabled': bool(TELEGRAM_TOKEN),
        'line_enabled': bool(LINE_CHANNEL_ACCESS_TOKEN)
    }), 200

# ============= MAIN =============

def main():
    """Start both Telegram and LINE bots"""
    print("=" * 50)
    print("🤖 Agent-Bot Starting")
    print("=" * 50)
    print(f"Telegram: {'✅ Enabled' if TELEGRAM_TOKEN else '❌ Disabled'}")
    print(f"LINE: {'✅ Enabled' if LINE_CHANNEL_ACCESS_TOKEN else '❌ Disabled'}")
    print(f"Authorized Users: {AUTHORIZED_USERS if AUTHORIZED_USERS != [''] else 'All (unrestricted)'}")
    print(f"Agent Path: {AGENT_PATH}")
    print("=" * 50)
    
    # Check if agent exists
    if not os.path.exists(AGENT_PATH):
        print(f"⚠️  WARNING: Agent not found at {AGENT_PATH}")
    
    # Start Telegram polling in background thread
    if TELEGRAM_TOKEN:
        telegram_thread = threading.Thread(target=telegram_polling, daemon=True)
        telegram_thread.start()
    
    # Start Flask for LINE webhook
    port = int(os.getenv('PORT', 5000))
    print(f"\n🌐 Starting Flask server on port {port}...")
    print(f"LINE webhook URL: https://agent-bot.fly.dev/webhook/line\n")
    
    app.run(host='0.0.0.0', port=port)

if __name__ == '__main__':
    main()
