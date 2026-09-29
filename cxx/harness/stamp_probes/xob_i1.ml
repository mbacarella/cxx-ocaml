module type S = sig
  open Set.Make(Bool)
  type nonrec t = t
end
let x = 1
