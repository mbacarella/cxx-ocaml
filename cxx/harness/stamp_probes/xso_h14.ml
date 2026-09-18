module type S = sig
  open Set.Make(Bool)
  type u = t constraint u = u
end
let x = 1
