module type S = sig
  open Set.Make(Bool)
  type u := int
end
let x = 1
