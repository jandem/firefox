/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at <http://mozilla.org/MPL/2.0/>. */

/* eslint no-shadow: ["error", { "allow": ["name"] }] */

import PropTypes from "resource://devtools/client/shared/vendor/react-prop-types.mjs";
import { span } from "resource://devtools/client/shared/vendor/react-dom-factories.mjs";

import {
  appendRTLClassNameIfNeeded,
  maybeEscapePropertyName,
  wrapRender,
} from "resource://devtools/client/shared/components/reps/reps/rep-utils.mjs";
import { MODE } from "resource://devtools/client/shared/components/reps/reps/constants.mjs";

/**
 * Property for Obj (local JS objects), Grip (remote JS objects)
 * and GripMap (remote JS maps and weakmaps) reps.
 * It's used to render object properties.
 */
PropRep.propTypes = {
  // Additional class to set on the key element
  keyClassName: PropTypes.string,
  // Property name.
  name: PropTypes.oneOfType([PropTypes.string, PropTypes.object]).isRequired,
  // Equal character rendered between property name and value.
  equal: PropTypes.string,
  mode: PropTypes.oneOf(Object.values(MODE)),
  onDOMNodeMouseOver: PropTypes.func,
  onDOMNodeMouseOut: PropTypes.func,
  onInspectIconClick: PropTypes.func,
  // Normally a PropRep will quote a property name that isn't valid
  // when unquoted; but this flag can be used to suppress the
  // quoting.
  suppressQuotes: PropTypes.bool,
  shouldRenderTooltip: PropTypes.bool,
};

/**
 * Function that given a name, a delimiter and an object returns an array
 * of React elements representing an object property (e.g. `name: value`)
 *
 * @param {Object} props
 * @return {Array} Array of React elements.
 */

function PropRep(props) {
  let { equal, keyClassName, mode, name, shouldRenderTooltip, suppressQuotes } =
    props;

  const className = `nodeName${keyClassName ? " " + keyClassName : ""}`;

  let key;
  // The key can be a simple string, for plain objects,
  // or another object for maps and weakmaps.
  if (typeof name === "string") {
    if (!suppressQuotes) {
      name = maybeEscapePropertyName(name);
    }
    key = span(
      {
        className: appendRTLClassNameIfNeeded(className, name),
        title: shouldRenderTooltip ? name : null,
      },
      name
    );
  } else {
    key = props.Rep({
      ...props,
      className,
      object: name,
      mode: mode || MODE.TINY,
      defaultRep: undefined,
    });
  }

  return [
    key,
    span(
      {
        className: "objectEqual",
      },
      equal
    ),
    props.Rep({ ...props }),
  ];
}

const rep = wrapRender(PropRep);

// Exports from this module
export default rep;
