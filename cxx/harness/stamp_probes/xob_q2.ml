open Set.Make(Int)
let x = 1
module M = struct module S = Set.Make(Int) end
