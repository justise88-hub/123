import json, base64, io
from pathlib import Path
import pytest
from PIL import Image
from card_designer import apply_element_changes, save_template_copy


def valid_design():
    b=io.BytesIO(); Image.new('RGB',(64,64),'white').save(b,format='PNG')
    uri='data:image/png;base64,'+base64.b64encode(b.getvalue()).decode()
    return {'front':{'elements':[{'fileName':'images.jpg','base64image':uri,'originalBase64Image':uri,'x':1,'y':2}, {'name':'Text','text':'Name','x':10,'y':20,'width':100,'height':30,'fontSize':12,'fill':'#000000'}]},'back':{'elements':[]},'about':{}}


def test_changes_preserve_unknown_fields():
    d=valid_design(); d['front']['elements'][1]['vendorExtra']={'x':1}
    out=apply_element_changes(d,'front',1,{'x':22,'text':'Иванов','fontSize':18})
    assert out['front']['elements'][1]['vendorExtra']=={'x':1}
    assert d['front']['elements'][1]['x']==10


def test_reject_unsafe_fields_and_invalid_geometry():
    d=valid_design()
    with pytest.raises(ValueError): apply_element_changes(d,'front',1,{'vendorExtra':1})
    with pytest.raises(ValueError): apply_element_changes(d,'front',1,{'width':0})


def test_save_copy_never_overwrites_source(tmp_path):
    d=valid_design(); source=tmp_path/'source.card'; target=tmp_path/'copy.card'
    source.write_text(json.dumps(d),encoding='utf-8')
    save_template_copy(source,target,d)
    assert source.read_text(encoding='utf-8') == json.dumps(d)
    assert target.exists()
    with pytest.raises(ValueError): save_template_copy(source,source,d)


def test_employee_text_tokens_are_substituted_only_when_marked():
    from card_personalization import personalize_design
    design = valid_design()
    design['front']['elements'][1]['text'] = 'ФИО: {{FULL_NAME}} / № {{BADGE_NUMBER}}'
    design['front']['elements'].append({'name':'Text','text':'Обычный текст без замены'})
    personalize_design(design, {'full_name':'Иванов Иван','badge_number':'0042'})
    assert design['front']['elements'][1]['text'] == 'ФИО: Иванов Иван / № 0042'
    assert design['front']['elements'][2]['text'] == 'Обычный текст без замены'


def test_unknown_personalization_token_is_preserved():
    from card_personalization import personalize_design
    design = valid_design()
    design['front']['elements'][1]['text'] = '{{UNKNOWN_FIELD}}'
    personalize_design(design, {})
    assert design['front']['elements'][1]['text'] == '{{UNKNOWN_FIELD}}'


def test_logo_replacement_preserves_element_geometry(tmp_path):
    from card_designer import replace_image_payload
    from PIL import Image
    design = valid_design()
    logo = tmp_path / 'logo.png'
    Image.new('RGBA', (80, 40), (10, 20, 30, 255)).save(logo)
    design['front']['elements'].append({'name':'Image','base64image':'old','originalBase64Image':'old','x':7,'y':8,'width':90,'height':45})
    updated = replace_image_payload(design, 'front', 2, logo)
    item = updated['front']['elements'][2]
    assert item['x'] == 7 and item['y'] == 8 and item['width'] == 90 and item['height'] == 45
    assert item['base64image'].startswith('data:image/png;base64,')
    assert design['front']['elements'][2]['base64image'] == 'old'


def test_logo_replacement_refuses_employee_portrait(tmp_path):
    from card_designer import replace_image_payload
    logo = tmp_path / 'logo.png'
    Image.new('RGB', (64, 64), 'white').save(logo)
    with pytest.raises(ValueError):
        replace_image_payload(valid_design(), 'front', 0, logo)


def test_add_text_element_clones_template_schema_without_mutating_source():
    from card_designer import add_text_element
    design = valid_design()
    design['front']['elements'][1]['vendorTextStyle'] = {'tracking': 2}
    updated, index = add_text_element(design, 'front', 'Должность: {{POSITION}}')
    added = updated['front']['elements'][index]
    assert added['text'] == 'Должность: {{POSITION}}'
    assert added['vendorTextStyle'] == {'tracking': 2}
    assert design['front']['elements'][-1]['text'] == 'Name'


def test_duplicate_and_remove_elements_keep_original_and_portrait_protected():
    from card_designer import duplicate_element, remove_element
    design = valid_design()
    duplicated, index = duplicate_element(design, 'front', 1)
    assert index == 2
    assert duplicated['front']['elements'][2]['x'] == 22
    assert len(design['front']['elements']) == 2
    removed = remove_element(duplicated, 'front', index)
    assert len(removed['front']['elements']) == 2
    with pytest.raises(ValueError):
        remove_element(design, 'front', 0)
    with pytest.raises(ValueError):
        duplicate_element(design, 'front', 0)


def test_editor_rejects_invalid_element_index_and_missing_text_prototype():
    from card_designer import add_text_element, remove_element
    with pytest.raises(IndexError):
        remove_element(valid_design(), 'front', 99)
    design = valid_design()
    design['front']['elements'] = [design['front']['elements'][0]]
    with pytest.raises(ValueError):
        add_text_element(design, 'front', 'Test')
