module type S = sig
  open Set.Make(Bool)
end
open Set.Make(Bool)
let e = empty
