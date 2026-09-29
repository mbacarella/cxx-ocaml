open Set.Make(Int)
let e = empty
module M = struct
  open Set.Make(String)
  let e = empty
end
