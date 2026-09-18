module type S = sig
  open Set.Make(Bool)
  type +_ u
  type v = t u
end
let x = 1
