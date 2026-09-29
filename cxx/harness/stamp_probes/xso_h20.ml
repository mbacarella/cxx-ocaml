module type S = sig
  open Set.Make(Bool)
  type u = < m : t >
end
let x = 1
