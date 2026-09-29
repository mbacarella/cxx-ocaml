module A = struct module N = struct let n = 1 end end
module B = struct include A end
