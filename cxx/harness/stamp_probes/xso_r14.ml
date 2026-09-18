module B = struct type t = bool let compare = compare end
module type S = sig
  open Set.Make(B)
  type u = t
end
let x = 1
