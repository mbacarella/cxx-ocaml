open Set.Make(Int)
let e = empty
module M = struct module S = Set.Make(Int) end
