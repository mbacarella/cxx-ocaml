module M = struct
  include Set.Make(Int) let e = add 1 empty let c = cardinal e end
let x = M.e
