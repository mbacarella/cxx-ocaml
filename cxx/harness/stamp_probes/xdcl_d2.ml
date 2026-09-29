module A = struct exception E of int end
module B = struct include A end
