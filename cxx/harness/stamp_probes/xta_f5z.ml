module F (X : sig end) = struct let a = 1 let b = 2 end
module TT = struct module N = F (struct end) end
