module type S = sig
  open Set.Make(Bool)
  type nonrec t = t
end
module type T = sig
  open Set.Make(Bool)
  type nonrec t = t
end
