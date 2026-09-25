module F (X : sig type t end) = struct type u = { f : X.t } [@@unboxed] type v = A of int end
module N = F (struct type t end)
let g (x : N.v) = match x with N.A n -> n
