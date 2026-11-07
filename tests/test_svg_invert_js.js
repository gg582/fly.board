'use strict';

const assert = require('node:assert/strict');
const images = require('../public/js/svg-invert.js');

function pixels(alpha) {
    const data = new Uint8ClampedArray(2 * 2 * 4);
    for (let i = 3; i < data.length; i += 4) data[i] = alpha;
    return data;
}

assert.equal(images.hasTransparentBackground(pixels(0), 2, 2), true);
assert.equal(images.hasTransparentBackground(pixels(255), 2, 2), false);
assert.equal(images.hasTransparentBackground(new Uint8ClampedArray(3), 2, 2), false);

function fixture(alpha, src = '/assets/uploads/diagram.svg', alt = '', formats = 'svg') {
    const listeners = {};
    const img = {
        dataset: {}, complete: true, naturalWidth: 2, naturalHeight: 2,
        currentSrc: src,
        getAttribute: name => name === 'src' ? src : name === 'alt' ? alt : null,
        addEventListener: (name, fn) => { listeners[name] = fn; }
    };
    const doc = {
        body: {
            getAttribute: name => name === 'data-invert-transparent-formats' ? formats : null,
            classList: {contains: name => name === 'transparent-image-invert-enabled'}
        },
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
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');
assert.equal(test.img.dataset.svgTransparencyChecked, 'checked');

test = fixture(255);
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'false');

test = fixture(0, '/assets/uploads/alpha.png');
assert.equal(images.inspectImage(test.img, test.doc), false);
assert.equal(test.img.dataset.svgTransparencyChecked, 'not-selected');

test = fixture(0, '/assets/uploads/alpha.png', '', 'svg,png');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');

test = fixture(255, '/assets/uploads/opaque.webp', '', 'webp');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'false');

test = fixture(0, '/assets/uploads/photo.jpeg', '', 'jpg');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');

test = fixture(0, 'data:image/png;base64,AA==', '', 'png');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');

test = fixture(0, 'https://oborona.zip/file/preview/242?poster=1', 'diagram.svg');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');
assert.equal(test.img.dataset.svgTransparencyChecked, 'checked');

test = fixture(255, '/file/preview/244?poster=1', 'opaque.svg');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'false');

test = fixture(0, '/file/preview/246?poster=1', 'alpha.png');
assert.equal(images.inspectImage(test.img, test.doc), false);
assert.equal(test.img.dataset.svgTransparencyChecked, 'not-selected');

test = fixture(0, '/file/preview/246?poster=1', 'alpha.png', 'png');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');

test = fixture(0, '/file/preview/248?poster=1', '', 'all');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'true');

test = fixture(255, '/assets/uploads/opaque.png', '', 'all');
assert.equal(images.inspectImage(test.img, test.doc), true);
assert.equal(test.img.dataset.svgTransparent, 'false');

test = fixture(0);
test.img.complete = false;
assert.equal(images.inspectImage(test.img, test.doc), false);
assert.equal(test.img.dataset.svgTransparencyChecked, 'pending');
test.img.complete = true;
test.listeners.load();
assert.equal(test.img.dataset.svgTransparent, 'true');

console.log('transparent image format and alpha detection tests passed');
