module type S = sig
  open Set.Make(Stdlib.Bool)
  type u = t
end
let x = 1
