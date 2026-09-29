module type S = sig
  open Set.Make(Bool)
  type u := t
end
let x = 1
