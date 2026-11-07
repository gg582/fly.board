'use strict';

const assert = require('node:assert/strict');
const svg = require('../public/js/svg-invert.js');

function pixels(alpha) {
    const data = new Uint8ClampedArray(2 * 2 * 4);
    for (let i = 3; i < data.length; i += 4) data[i] = alpha;
    return data;
}

assert.equal(svg.hasTransparentBackground(pixels(0), 2, 2), true);
assert.equal(svg.hasTransparentBackground(pixels(255), 2, 2), false);
assert.equal(svg.hasTransparentBackground(new Uint8ClampedArray(3), 2, 2), false);

function fixture(alpha, src = '/assets/uploads/diagram.svg', alt = '') {
    const listeners = {};
    const img = {
        dataset: {}, complete: true, naturalWidth: 2, naturalHeight: 2,
        currentSrc: src,
        getAttribute: name => name === 'src' ? src : name === 'alt' ? alt : null,
        addEventListener: (name, fn) => { listeners[name] = fn; }
    };
    const doc = {
        createElement: () => ({
            getContext: () => ({
                drawImage: () => {},
                getImageData: () => ({data: pixels(alpha)})
            })
        })
    };
    return {img, doc, listeners};
}

let test = fixture(0);
assert.equal(svg.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');
assert.equal(test.img.dataset.svgTransparencyChecked, 'checked');

test = fixture(255);
assert.equal(svg.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'false');

test = fixture(0, '/assets/uploads/photo.png');
assert.equal(svg.inspectImage(test.img, test.doc), false);
assert.equal(test.img.dataset.svgTransparencyChecked, 'not-svg');

test = fixture(0, 'https://oborona.zip/file/preview/242?poster=1', 'diagram.svg');
assert.equal(svg.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');
assert.equal(test.img.dataset.svgTransparencyChecked, 'checked');

test = fixture(255, '/file/preview/244?poster=1', 'opaque.svg');
assert.equal(svg.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'false');

test = fixture(0, '/file/preview/246?poster=1', 'photo.webp');
assert.equal(svg.inspectImage(test.img, test.doc), false);
assert.equal(test.img.dataset.svgTransparencyChecked, 'not-svg');

test = fixture(0);
test.img.complete = false;
assert.equal(svg.inspectImage(test.img, test.doc), false);
assert.equal(test.img.dataset.svgTransparencyChecked, 'pending');
test.img.complete = true;
test.listeners.load();
assert.equal(test.img.dataset.svgTransparent, 'true');

console.log('transparent SVG detection tests passed');
