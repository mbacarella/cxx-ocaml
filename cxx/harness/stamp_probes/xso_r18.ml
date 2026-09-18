module type S = sig
  open MoreLabels.Set.Make(Bool)
  type u = t
end
let x = 1
