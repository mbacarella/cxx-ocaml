module F (X : sig type t end) = struct
  type 'a u = U : 'a -> 'a u | V : X.t -> int u
  let f (x : 'a u) = x
end
module P = struct type t = int end
open F(P)
let g x = ignore (match x with U _ -> 1 | _ -> 2)
