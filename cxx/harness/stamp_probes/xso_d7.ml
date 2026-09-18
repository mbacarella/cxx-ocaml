module M = Set.Make(Bool)
module type S = sig
  open Set.Make(Bool)
  type nonrec t = t
end
