export const description = `
Atomically read, max and store value.

* Load the original value pointed to by atomic_ptr.
* Obtains a new value by taking the max with the value v.
* Store the new value using atomic_ptr.

Returns the original value stored in the atomic object.
`;

import { makeTestGroup } from '../../../../../../../common/framework/test_group.js';
import { AllFeaturesMaxLimitsGPUTest } from '../../../../../../gpu_test.js';

import {
  dispatchSizes,
  workgroupSizes,
  runStorageVariableTest,
  runWorkgroupVariableTest,
  typedArrayCtor,
} from './harness.js';

export const g = makeTestGroup(AllFeaturesMaxLimitsGPUTest);

g.test('max_storage')
  .specURL('https://www.w3.org/TR/WGSL/#atomic-rmw')
  .desc(
    `
AS is storage or workgroup
T is i32 or u32

fn atomicMax(atomic_ptr: ptr<AS, atomic<T>, read_write>, v: T) -> T
`
  )
  .params(u =>
    u
      .combine('workgroupSize', workgroupSizes)
      .combine('dispatchSize', dispatchSizes)
      .combine('scalarType', ['u32', 'i32'] as const)
  )
  .fn(t => {
    const numInvocations = t.params.workgroupSize * t.params.dispatchSize;
    // Allocate one extra element to ensure it doesn't get modified
    const bufferNumElements = 2;

    const initValue = 0;
    const op = `atomicMax(&output[0], id)`;
    const expected = new (typedArrayCtor(t.params.scalarType))(bufferNumElements);
    expected[0] = numInvocations - 1;

    runStorageVariableTest({
      t,
      workgroupSize: t.params.workgroupSize,
      dispatchSize: t.params.dispatchSize,
      bufferNumElements,
      initValue,
      op,
      expected,
    });
  });

g.test('max_workgroup')
  .specURL('https://www.w3.org/TR/WGSL/#atomic-rmw')
  .desc(
    `
AS is storage or workgroup
T is i32 or u32

fn atomicMax(atomic_ptr: ptr<AS, atomic<T>, read_write>, v: T) -> T
`
  )
  .params(u =>
    u
      .combine('workgroupSize', workgroupSizes)
      .combine('dispatchSize', dispatchSizes)
      .combine('scalarType', ['u32', 'i32'] as const)
  )
  .fn(t => {
    // Allocate one extra element to ensure it doesn't get modified
    const wgNumElements = 2;

    const initValue = 0;
    const op = `atomicMax(&wg[0], id)`;

    const expected = new (typedArrayCtor(t.params.scalarType))(
      wgNumElements * t.params.dispatchSize
    ).fill(initValue);
    for (let d = 0; d < t.params.dispatchSize; ++d) {
      const wg = expected.subarray(d * wgNumElements);
      wg[0] = t.params.workgroupSize - 1;
    }

    runWorkgroupVariableTest({
      t,
      workgroupSize: t.params.workgroupSize,
      dispatchSize: t.params.dispatchSize,
      wgNumElements,
      initValue,
      op,
      expected,
    });
  });
