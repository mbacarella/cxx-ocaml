module type S = sig
  open Set.Make(Bool)
  type u = { f : t }
end
let x = 1
