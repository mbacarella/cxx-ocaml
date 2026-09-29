open Bigarray
let special_complex32 (a: (Complex.t, complex32_elt, c_layout) Array1.t)
                      v0 v1 v2 =
  a.{0} <- v0; a.{1} <- v1; a.{2} <- v2;
  (a.{0}, a.{1}, a.{2})
