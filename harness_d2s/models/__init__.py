"""Model defaults, independent of document parsing presets."""
import json
import re
from importlib.resources import files


def load(model='luna'):
    if not isinstance(model,str):
        raise ValueError('Use a model profile name or a provider/model ID')
    if model=='openai/gpt-6-luna':
        model='luna'
    if re.fullmatch(r'[A-Za-z0-9_-]+',model):
        profile=files(__package__).joinpath(model+'.json')
        if profile.is_file():
            return json.loads(profile.read_text(encoding='utf-8'))
    if '/' not in model:
        raise ValueError(f'Unknown model profile: {model}')
    return {'model':model,'service_tier':'default'}
