// Estimate total work units for a small agent task.
capability math.double;
let agents = 3;
let tasks_per_agent = 7;
let first_pass = agents * tasks_per_agent;
return tool math.double(first_pass);
