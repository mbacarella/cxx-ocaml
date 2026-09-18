module type S = sig
  open Set.Make(Bool)
  type nonrec t = t
  type u = t
end
