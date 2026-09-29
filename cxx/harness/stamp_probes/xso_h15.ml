module type S = sig
  open Set.Make(Bool)
  type u = { f : t list }
end
let x = 1
