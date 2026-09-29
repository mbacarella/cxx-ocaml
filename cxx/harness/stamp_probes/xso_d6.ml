type u = Set.Make(Bool).t
module type S = sig
  open Set.Make(Bool)
  type nonrec t = t
end
