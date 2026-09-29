open Bigarray
let special_complex32 (a: (Complex.t, complex32_elt, c_layout) Array1.t) v0 =
  a.{0} <- v0
