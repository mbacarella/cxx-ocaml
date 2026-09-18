open Set.Make(Int)
let e = empty
module type S = sig
  open Set.Make(Bool)
  type nonrec t = t
end
