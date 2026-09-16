module type E = sig exception Ex end
module M = struct exception Ex end
let f (M.Ex | _) = "42"
