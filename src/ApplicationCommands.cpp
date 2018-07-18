#include "ApplicationCommands.h"

#include "Logging.h"
#include "Utilities.h"

crow::response listApplications(PersistentStore& store, const crow::request& req){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to list applications");
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//All users are allowed to list applications

	std::string repoName="slate";
	if(req.url_params.get("dev"))
		repoName="slate-dev";
	
	auto commandResult=runCommand("helm search "+repoName+"/");
	std::vector<std::string> lines = string_split_lines(commandResult);

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);

	rapidjson::Value resultItems(rapidjson::kArrayType);
        int n = 0;
	while (n < lines.size()) {
		if (n > 0) {
			auto tokens = string_split_columns(lines[n], '\t');
	  	    
			rapidjson::Value applicationResult(rapidjson::kObjectType);
			applicationResult.AddMember("apiVersion", "v1alpha1", alloc);
			applicationResult.AddMember("kind", "Application", alloc);
			rapidjson::Value applicationData(rapidjson::kObjectType);

			rapidjson::Value name;
			name.SetString(tokens[0].c_str(), tokens[0].length(), alloc);
			applicationData.AddMember("name", name, alloc);
			rapidjson::Value app_version;
			app_version.SetString(tokens[2].c_str(), tokens[2].length(), alloc);
			applicationData.AddMember("app_version", app_version, alloc);
			rapidjson::Value chart_version;
			chart_version.SetString(tokens[1].c_str(), tokens[1].length(), alloc);
			applicationData.AddMember("chart_version", chart_version, alloc);
			rapidjson::Value description;
			description.SetString(tokens[3].c_str(), tokens[3].length(), alloc);
			applicationData.AddMember("description", description, alloc);
	    
			applicationResult.AddMember("metadata", applicationData, alloc);
			resultItems.PushBack(applicationResult, alloc);
		}
		n++;
	}

	result.AddMember("items", resultItems, alloc);

	return crow::response(to_string(result));
}

crow::response fetchApplicationConfig(PersistentStore& store, const crow::request& req, const std::string& appName){
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to fetch configuration for application " << appName);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	//TODO: Can all users obtain configurations for all applications?

	std::string repoName = "slate";
	if(req.url_params.get("dev"))
		repoName = "slate-dev";
		
	auto commandResult = runCommand("helm inspect " + repoName + "/" + appName);

	if (commandResult.find("Error") != std::string::npos)
		return crow::response(404, generateError("Application not found"));

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	result.AddMember("kind", "Configuration", alloc);

	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("name", rapidjson::StringRef(appName.c_str()), alloc);
	result.AddMember("metadata", metadata, alloc);

	rapidjson::Value spec(rapidjson::kObjectType);
	spec.AddMember("body", rapidjson::StringRef(commandResult.c_str()), alloc);
	result.AddMember("spec", spec, alloc);

	return crow::response(to_string(result));
}

Application findApplication(std::string appName, Application::Repository repo){
	std::string repoName="slate";
	if(repo==Application::DevelopmentRepository)
		repoName="slate-dev";
	auto command="helm search "+repoName+"/"+appName;
	auto result=runCommand(command);
	
	if(result.find("No results found")!=std::string::npos)
		return Application();
	
	//TODO: deal with the possibility of multiple results, which could happen if
	//both "slate/stuff" and "slate/superduper" existed and the user requested
	//the application "s". Multiple results might also not indicate ambiguity, 
	//if the user searches for the full name of an application, which is also a
	//prefix of the name another application which exists
	
	return Application(appName);
}

crow::response installApplication(PersistentStore& store, const crow::request& req, const std::string& appName){
	if(appName.find('\'')!=std::string::npos)
		return crow::response(400,generateError("Application names cannot contain single quote characters"));
	Application::Repository repo=Application::MainRepository;
	if(req.url_params.get("dev"))
		repo=Application::DevelopmentRepository;
	const Application application=findApplication(appName,repo);
	if(!application)
		return crow::response(404,generateError("Application not found"));
	
	const User user=authenticateUser(store, req.url_params.get("token"));
	log_info(user << " requested to install an instance of " << application);
	if(!user)
		return crow::response(403,generateError("Not authorized"));
	
	//collect data out of JSON body
	rapidjson::Document body;
	try{
		body.Parse(req.body.c_str());
	}catch(std::runtime_error& err){
		return crow::response(400,generateError("Invalid JSON in request body"));
	}
	if(body.IsNull())
		return crow::response(400,generateError("Invalid JSON in request body"));
	
	if(!body.HasMember("vo"))
		return crow::response(400,generateError("Missing VO"));
	if(!body["vo"].IsString())
		return crow::response(400,generateError("Incorrect type for VO"));
	const std::string voID=body["vo"].GetString();
	
	if(!body.HasMember("cluster"))
		return crow::response(400,generateError("Missing cluster"));
	if(!body["cluster"].IsString())
		return crow::response(400,generateError("Incorrect type for cluster"));
	const std::string clusterID=body["cluster"].GetString();
	
	if(!body.HasMember("tag"))
		return crow::response(400,generateError("Missing tag"));
	if(!body["tag"].IsString())
		return crow::response(400,generateError("Incorrect type for tag"));
	const std::string tag=body["tag"].GetString();
	
	if(!body.HasMember("configuration"))
		return crow::response(400,generateError("Missing configuration"));
	if(!body["configuration"].IsString())
		return crow::response(400,generateError("Incorrect type for configuration"));
	const std::string config=body["configuration"].GetString();
	
	//validate input
	const VO vo=store.getVO(voID);
	if(!vo)
		return crow::response(400,generateError("Invalid VO"));
	const Cluster cluster=store.getCluster(clusterID);
	if(!cluster)
		return crow::response(400,generateError("Invalid Cluster"));
	//A user must belong to a VO to install applications on its behalf
	if(!store.userInVO(user.id,vo.id))
		return crow::response(403,generateError("Not authorized"));
	
	ApplicationInstance instance;
	instance.valid=true;
	instance.id=idGenerator.generateInstanceID();
	instance.application=appName;
	instance.owningVO=vo.id;
	instance.cluster=cluster.id;
	//TODO: strip comments and whitespace from config
	instance.config=reduceYAML(config);
	if(instance.config.empty())
		instance.config="\n"; //empty strings upset Dynamo
	instance.ctime=timestamp();
	instance.name=appName;
	if(!tag.empty())
		instance.name+="-"+tag;
	if(instance.name.size()>63)
		return crow::response(400,generateError("Instance tag too long"));
	
	log_info("Instantiating " << application  << " on " << cluster);
	//first record the instance in the peristent store
	bool success=store.addApplicationInstance(instance);
	
	if(!success){
		return crow::response(500,generateError("Failed to add application instance"
												" record the persistent store"));
	}

	std::string repoName = "slate";
	if (repo == Application::DevelopmentRepository)
		repoName = "slate-dev";

	auto configPath=store.configPathForCluster(cluster.id);
	std::string commandResult;
	commandResult = runCommand("export KUBECONFIG='"+*configPath+
	                           "'; helm install " + repoName + "/" + 
	                           application.name + " --name " + instance.name + 
							   " --namespace " + vo.namespaceName());
	
	//if application instantiation fails, remove record from DB again
	if(commandResult.find("STATUS: DEPLOYED")==std::string::npos){
		std::string errMsg="Failed to start application instance with helm";
		//try to figure out what helm is unhappy about to tell the user
		for(auto line : string_split_lines(commandResult)){
			if(line.find("Error")){
				errMsg+=": "+line;
				break;
			}
		}
		store.removeApplicationInstance(instance.id);
		//TODO: include any other error information?
		return crow::response(500,generateError(errMsg));
	}

	auto listResult = runCommand("helm list " + instance.name);
	auto lines = string_split_lines(listResult);
	auto cols = string_split_columns(lines[1], '\t');

	rapidjson::Document result(rapidjson::kObjectType);
	rapidjson::Document::AllocatorType& alloc = result.GetAllocator();
	
	result.AddMember("apiVersion", "v1alpha1", alloc);
	result.AddMember("kind", "Configuration", alloc);
	rapidjson::Value metadata(rapidjson::kObjectType);
	metadata.AddMember("id", rapidjson::StringRef(instance.id.c_str()), alloc);
	metadata.AddMember("name", rapidjson::StringRef(instance.name.c_str()), alloc);
	metadata.AddMember("revision", rapidjson::StringRef(cols[1].c_str()), alloc);
	metadata.AddMember("updated", rapidjson::StringRef(cols[2].c_str()), alloc);
	metadata.AddMember("application", rapidjson::StringRef(appName.c_str()), alloc);
	metadata.AddMember("vo", rapidjson::StringRef(vo.id.c_str()), alloc);
	result.AddMember("metadata", metadata, alloc);
	result.AddMember("status", "DEPLOYED", alloc);

	return crow::response(to_string(result));
}
