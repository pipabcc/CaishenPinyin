import os
import math
from PIL import Image, ImageDraw

def create_rounded_rect_slice(width, height, radius, bg_color):
    """生成无描边、上下同色的 32 位 RGBA 九宫格位图。"""
    scale = 4
    w, h, r = width * scale, height * scale, radius * scale
    img = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    draw = ImageDraw.Draw(img)
    draw.rounded_rectangle([0, 0, w - 1, h - 1], radius=r, fill=bg_color)
        
    # 缩小超采样平滑
    return img.resize((width, height), Image.Resampling.LANCZOS)

def generate_skins():
    base_dir = os.path.join(os.path.dirname(os.path.dirname(__file__)), 'data', 'skins')
    os.makedirs(base_dir, exist_ok=True)
    
    skins_def = [
        {
            'id': 'classic_blue',
            'name': '经典蓝调',
            'author': '财神输入法官方',
            'version': '1.0',
            'info': '原版纯净白底搭配科技亮蓝高光胶囊，经典耐看、高对比度清晰视效',
            'bg_color': (255, 255, 255, 252),
            'font_size': 18,
            'pinyin_color': '0x000000',       # 纯黑粗体拼音
            'candidate_color': '0x000000',    # 纯黑候选词
            'highlight_color': '0xFFFFFF',    # 纯白
            'highlight_bg_color': '0x2F6BFF', # 经典科技亮蓝胶囊
            'index_color': '0x000000',        # 序号
            'status_text_color': '0x8A94A3',  # 灰度统计字数
            'separator_color': '0xE6E9EF',    # 浅灰细分隔线
            'corner_radius': 6,
        },
        {
            'id': 'classic_gold',
            'name': '财神金韵',
            'author': '财神输入法官方',
            'version': '1.0',
            'info': '暖白温润底色配流金与朱红点缀，典雅高贵的经典国风主题',
            'bg_color': (255, 253, 248, 252),
            'font_size': 18,
            'pinyin_color': '0x8B1E0F',       # 朱红偏深
            'candidate_color': '0x2C2416',    # 墨黑暖调
            'highlight_color': '0xFFFFFF',    # 纯白
            'highlight_bg_color': '0xC8161D', # 财神正红
            'index_color': '0xB8860B',        # 暗金
            'status_text_color': '0x998877',
            'separator_color': '0xF0E4D2',
            'corner_radius': 8,
        },
        {
            'id': 'minimal_light',
            'name': '极简无界',
            'author': '财神输入法官方',
            'version': '1.0',
            'info': '纯净透亮磨砂白，极简无边框浅灰，专为日常与高效办公设计',
            'bg_color': (255, 255, 255, 250),
            'font_size': 18,
            'pinyin_color': '0x0F172A',
            'candidate_color': '0x1E293B',
            'highlight_color': '0xFFFFFF',
            'highlight_bg_color': '0x0F172A', # 极简深空黑灰
            'index_color': '0x64748B',
            'status_text_color': '0x94A3B8',
            'separator_color': '0xE2E8F0',
            'corner_radius': 8,
        },
        {
            'id': 'cyber_dark',
            'name': '暗黑极客',
            'author': '财神输入法官方',
            'version': '1.0',
            'info': '深空暗夜背景与冰蓝霓虹高亮，极客程序员的专属护眼暗色主题',
            'bg_color': (26, 27, 38, 250),
            'font_size': 18,
            'pinyin_color': '0x7AA2F7',       # 冰蓝
            'candidate_color': '0xC0CAF5',    # 浅银蓝
            'highlight_color': '0x1A1B26',    # 选中文字深色
            'highlight_bg_color': '0x7AA2F7', # 选中背景冰蓝
            'index_color': '0x565F89',        # 灰蓝序号
            'status_text_color': '0x787C99',
            'separator_color': '0x2F354A',
            'corner_radius': 8,
        },
        {
            'id': 'sakura_pink',
            'name': '春日樱粉',
            'author': '财神输入法官方',
            'version': '1.0',
            'info': '温柔浅粉与初樱点缀，治愈甜美的马卡龙二次元风格',
            'bg_color': (255, 245, 247, 252),
            'font_size': 18,
            'pinyin_color': '0xD81B60',       # 樱桃深粉
            'candidate_color': '0x4A3E3D',    # 柔和暖褐
            'highlight_color': '0xFFFFFF',
            'highlight_bg_color': '0xF06292', # 樱花粉
            'index_color': '0xBA68C8',        # 浅紫粉序号
            'status_text_color': '0xCE93D8',
            'separator_color': '0xF8BBD0',
            'corner_radius': 8,
        },
        {
            'id': 'celadon_jade',
            'name': '碧水青瓷',
            'author': '财神输入法官方',
            'version': '1.0',
            'info': '雨过天晴云破处的淡雅天青色，搭配竹青黛绿，诗意盎然',
            'bg_color': (240, 249, 248, 252),
            'font_size': 18,
            'pinyin_color': '0x00695C',       # 墨绿/竹青
            'candidate_color': '0x1B3834',    # 黛绿墨色
            'highlight_color': '0xFFFFFF',
            'highlight_bg_color': '0x00897B', # 碧绿青瓷
            'index_color': '0x4DB6AC',        # 天青序号
            'status_text_color': '0x80CBC4',
            'separator_color': '0xB2DFDB',
            'corner_radius': 8,
        }
    ]
    
    for s in skins_def:
        skin_dir = os.path.join(base_dir, s['id'])
        os.makedirs(skin_dir, exist_ok=True)
        
        # 1. 生成 9-Slice 背景位图 (72x48, 四周保留 16px 切片)
        bg_img = create_rounded_rect_slice(
            72, 48, radius=s['corner_radius'],
            bg_color=s['bg_color']
        )
        bg_img.save(os.path.join(skin_dir, 'cand_bg.png'), 'PNG')
        
        # 2. 生成 skin.ini
        ini_content = f"""[General]
name={s['name']}
author={s['author']}
version={s['version']}
info={s['info']}

[Display]
font_family=Microsoft YaHei UI
font_size={s['font_size']}
pinyin_color={s['pinyin_color']}
candidate_color={s['candidate_color']}
highlight_color={s['highlight_color']}
highlight_bg_color={s['highlight_bg_color']}
index_color={s['index_color']}
status_text_color={s['status_text_color']}
separator_color={s['separator_color']}

[Scheme_H1]
bg_image=cand_bg.png
layout_horizontal=0,16,16
layout_vertical=0,16,16
pinyin_margin=10,6,16,16
candidate_margin=6,10,16,16
corner_radius={s['corner_radius']}
has_shadow=1
"""
        with open(os.path.join(skin_dir, 'skin.ini'), 'w', encoding='utf-8') as f:
            f.write(ini_content)
            
        print(f"[OK] Generated skin: {s['name']} ({s['id']}) in {skin_dir}")

if __name__ == '__main__':
    generate_skins()
