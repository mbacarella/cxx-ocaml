module N0 = struct module N = struct let x = 1 let y = 2 end end
module TT = struct include N0 end
