module S = Set.Make(String)
module type T = sig val v : Set.Make(String).t end
